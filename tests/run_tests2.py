import base64
import binascii
import glob
import os
import subprocess
from typing import Optional, Tuple, Dict, Any

# NOTE: multiprocess has a similar interface to multiprocessing,
#       but can sometimes handle more complex data structures.
#       Make sure you have it installed, e.g. pip install multiprocess
from multiprocess import Pool

from colorama import Fore, Style, init


def get_base64_size(s: str) -> Optional[int]:
    """Return the size of the data decoded from a base64 string s, or None if invalid."""
    try:
        decoded = base64.b64decode(s, validate=True)
    except binascii.Error:
        return None
    return len(decoded)


def process_test(
    args: Tuple[int, str, int, int]
) -> Dict[str, Any]:
    """
    Process a single test file by:
    1) Reading the base64 data
    2) Running compression
    3) Running decompression
    4) Computing the points if successful
    Returns a dict with all info needed for printing & summarizing.
    """
    i, test_file, name_width, n_tests = args

    with open(test_file, "r") as f:
        original_block = f.read().split()[1]

    original_size = get_base64_size(original_block)
    if original_size is None or original_size > 2 ** 21:
        return {
            "index": i,
            "test_file": test_file,
            "message": f"{Fore.RED}PE invalid or too large original base64{Style.RESET_ALL}",
            "success": False,
            "compressed_size": None,
            "points": 0.0,
            "original_size": original_size,
        }

    # Prepare partial output for the result line
    # We store these so we can print them in the main thread in consistent formatting.
    index_str = f"{Fore.YELLOW}{i:>4}{Style.RESET_ALL}"
    size_str = f"{Fore.CYAN}{original_size:>7}{Style.RESET_ALL}"

    # 1) Compress
    try:
        result = subprocess.run(
            "../build/solution",
            input="compress\n" + original_block,
            text=True,
            capture_output=True,
            timeout=2.0,
        )
    except subprocess.TimeoutExpired:
        return {
            "index": i,
            "test_file": test_file,
            "message": f"{Fore.RED}TL timeout expired{Style.RESET_ALL}",
            "success": False,
            "compressed_size": None,
            "points": 0.0,
            "original_size": original_size,
        }
    if result.returncode != 0:
        return {
            "index": i,
            "test_file": test_file,
            "message": f"{Fore.RED}RE exitcode={result.returncode}{Style.RESET_ALL}",
            "success": False,
            "compressed_size": None,
            "points": 0.0,
            "original_size": original_size,
        }

    compressed_block = result.stdout.strip()
    compressed_size = get_base64_size(compressed_block)
    if compressed_size is None:
        return {
            "index": i,
            "test_file": test_file,
            "message": f"{Fore.RED}PE invalid base64{Style.RESET_ALL}",
            "success": False,
            "compressed_size": None,
            "points": 0.0,
            "original_size": original_size,
        }
    if compressed_size > 2 ** 21:
        return {
            "index": i,
            "test_file": test_file,
            "message": f"{Fore.RED}PE too big output ({compressed_size}){Style.RESET_ALL}",
            "success": False,
            "compressed_size": compressed_size,
            "points": 0.0,
            "original_size": original_size,
        }

    # 2) Decompress
    try:
        result = subprocess.run(
            "../build/solution",
            input="decompress\n" + compressed_block,
            text=True,
            capture_output=True,
            timeout=2.0,
        )
    except subprocess.TimeoutExpired:
        return {
            "index": i,
            "test_file": test_file,
            "message": f"{Fore.RED}TL timeout expired{Style.RESET_ALL}",
            "success": False,
            "compressed_size": compressed_size,
            "points": 0.0,
            "original_size": original_size,
        }
    if result.returncode != 0:
        return {
            "index": i,
            "test_file": test_file,
            "message": f"{Fore.RED}RE exitcode={result.returncode}{Style.RESET_ALL}",
            "success": False,
            "compressed_size": compressed_size,
            "points": 0.0,
            "original_size": original_size,
        }

    decompressed_block = result.stdout.strip()
    if decompressed_block != original_block:
        return {
            "index": i,
            "test_file": test_file,
            "message": f"{Fore.RED}WA wrong decompressed block{Style.RESET_ALL}",
            "success": False,
            "compressed_size": compressed_size,
            "points": 0.0,
            "original_size": original_size,
        }

    # If all OK, compute points
    points = 1000.0 * (2 * original_size) / (original_size + compressed_size)
    comp_str = f"{Fore.GREEN}OK {Fore.CYAN}{compressed_size:>7}{Style.RESET_ALL}"
    decomp_str = f"{Fore.GREEN}OK{Style.RESET_ALL} {Fore.CYAN}{points:9.3f}{Style.RESET_ALL}"

    message = (
        f"{index_str} {test_file:<{name_width}} {size_str}   "
        + f"{comp_str:18} {decomp_str}"
    )

    return {
        "index": i,
        "test_file": test_file,
        "message": message,
        "success": True,
        "compressed_size": compressed_size,
        "points": points,
        "original_size": original_size,
    }


def main():
    init(autoreset=True)

    # Gather test files
    test_files = glob.glob("cases/*.txt")
    test_files.sort()
    n_tests = len(test_files)
    if n_tests == 0:
        print("Error: no tests")
        exit(2)

    name_width = max(max(len(s) for s in test_files), 4)

    print(f"{Fore.YELLOW}====================== Running {n_tests} tests ======================{Style.RESET_ALL}")
    print(f"{Fore.YELLOW}Idx  {'Name':<{name_width}} {'Size':>7}   Compression   Decompression {Style.RESET_ALL}")

    # Prepare arguments for each test
    args_list = [
        (i + 1, test_file, name_width, n_tests)
        for i, test_file in enumerate(test_files)
    ]

    # Run in parallel
    with Pool() as pool:
        results = pool.map(process_test, args_list)

    # Sort results by index (map keeps order, but just to be sure)
    # (If you rely on map's guarantee of ordering, you can skip sorting)
    results.sort(key=lambda x: x["index"])

    n_ok = 0
    total_points = 0.0

    # Print the per-test results
    for res in results:
        # If process_test encountered an error, it constructed a short error message
        if "message" in res:
            print(res["message"])
        else:
            print(f"{Fore.RED}Unknown error for test index {res['index']}{Style.RESET_ALL}")

        if res.get("success", False):
            n_ok += 1
            total_points += res["points"]

    # Final summary
    if n_ok == n_tests:
        passed_color = Fore.GREEN
    else:
        passed_color = Fore.RED

    print(f"{Fore.YELLOW}Passed tests:   {passed_color}{n_ok}/{n_tests}{Style.RESET_ALL}")
    print(f"{Fore.YELLOW}Average points: {Fore.CYAN}{(total_points / n_tests):.3f}{Style.RESET_ALL}")
    print(f"{Fore.YELLOW}Total points:   {Fore.CYAN}{total_points:.3f}{Style.RESET_ALL}")


if __name__ == "__main__":
    main()
