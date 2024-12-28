import argparse
import base64
import binascii
import glob
import os
import subprocess
from typing import Optional, Tuple, Dict, Any

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
    args: Tuple[int, str, int, int, str]
) -> Dict[str, Any]:
    """
    Process a single test file by:
    1) Reading the base64 data
    2) Running compression
    3) Running decompression
    4) Computing the points if successful
    Returns a dict with all info needed for printing & summarizing.
    
    :param args: A tuple containing:
        - i (int): Index of this test (1-based)
        - test_file (str): Path to the test file
        - name_width (int): Max width for the test name (for aligned printing)
        - n_tests (int): Total number of tests
        - bin_path (str): Path to the solution binary
    """
    i, test_file, name_width, n_tests, bin_path = args

    index_str = f"{Fore.YELLOW}{i:>4}{Style.RESET_ALL}"

    try:
        with open(test_file, "r") as f:
            original_block = f.read().split()[1]
    except Exception as e:
        size_str = f"{Fore.CYAN}{'?':>7}{Style.RESET_ALL}"
        message = (
            f"{index_str} {test_file:<{name_width}} {size_str}   "
            f"{Fore.RED}PE failed to read file: {e}{Style.RESET_ALL}"
        )
        return {
            "index": i,
            "test_file": test_file,
            "message": message,
            "success": False,
            "compressed_size": None,
            "points": 0.0,
            "original_size": None,
        }

    original_size = get_base64_size(original_block)
    if original_size is None:
        size_str = f"{Fore.CYAN}{'?':>7}{Style.RESET_ALL}"
        message = (
            f"{index_str} {test_file:<{name_width}} {size_str}   "
            f"{Fore.RED}PE invalid base64 in file{Style.RESET_ALL}"
        )
        return {
            "index": i,
            "test_file": test_file,
            "message": message,
            "success": False,
            "compressed_size": None,
            "points": 0.0,
            "original_size": None,
        }

    size_str = f"{Fore.CYAN}{original_size:>7}{Style.RESET_ALL}"

    if original_size > 2 ** 21:
        message = (
            f"{index_str} {test_file:<{name_width}} {size_str}   "
            f"{Fore.RED}PE original input too large ({original_size}){Style.RESET_ALL}"
        )
        return {
            "index": i,
            "test_file": test_file,
            "message": message,
            "success": False,
            "compressed_size": None,
            "points": 0.0,
            "original_size": original_size,
        }

    # 1) Compress
    try:
        result = subprocess.run(
            bin_path,
            input="compress\n" + original_block,
            text=True,
            capture_output=True,
            timeout=2.0,
        )
    except subprocess.TimeoutExpired:
        message = (
            f"{index_str} {test_file:<{name_width}} {size_str}   "
            f"{Fore.RED}TL timeout expired (compression){Style.RESET_ALL}"
        )
        return {
            "index": i,
            "test_file": test_file,
            "message": message,
            "success": False,
            "compressed_size": None,
            "points": 0.0,
            "original_size": original_size,
        }
    if result.returncode != 0:
        message = (
            f"{index_str} {test_file:<{name_width}} {size_str}   "
            f"{Fore.RED}RE exitcode={result.returncode} (compression){Style.RESET_ALL}"
        )
        return {
            "index": i,
            "test_file": test_file,
            "message": message,
            "success": False,
            "compressed_size": None,
            "points": 0.0,
            "original_size": original_size,
        }

    compressed_block = result.stdout.strip()
    compressed_size = get_base64_size(compressed_block)
    if compressed_size is None:
        message = (
            f"{index_str} {test_file:<{name_width}} {size_str}   "
            f"{Fore.RED}PE invalid base64 (compression output){Style.RESET_ALL}"
        )
        return {
            "index": i,
            "test_file": test_file,
            "message": message,
            "success": False,
            "compressed_size": None,
            "points": 0.0,
            "original_size": original_size,
        }
    if compressed_size > 2 ** 21:
        message = (
            f"{index_str} {test_file:<{name_width}} {size_str}   "
            f"{Fore.RED}PE too big output ({compressed_size}){Style.RESET_ALL}"
        )
        return {
            "index": i,
            "test_file": test_file,
            "message": message,
            "success": False,
            "compressed_size": compressed_size,
            "points": 0.0,
            "original_size": original_size,
        }

    # 2) Decompress
    try:
        result = subprocess.run(
            bin_path,
            input="decompress\n" + compressed_block,
            text=True,
            capture_output=True,
            timeout=2.0,
        )
    except subprocess.TimeoutExpired:
        message = (
            f"{index_str} {test_file:<{name_width}} {size_str}   "
            f"{Fore.RED}TL timeout expired (decompression){Style.RESET_ALL}"
        )
        return {
            "index": i,
            "test_file": test_file,
            "message": message,
            "success": False,
            "compressed_size": compressed_size,
            "points": 0.0,
            "original_size": original_size,
        }
    if result.returncode != 0:
        message = (
            f"{index_str} {test_file:<{name_width}} {size_str}   "
            f"{Fore.RED}RE exitcode={result.returncode} (decompression){Style.RESET_ALL}"
        )
        return {
            "index": i,
            "test_file": test_file,
            "message": message,
            "success": False,
            "compressed_size": compressed_size,
            "points": 0.0,
            "original_size": original_size,
        }

    decompressed_block = result.stdout.strip()
    if decompressed_block != original_block:
        message = (
            f"{index_str} {test_file:<{name_width}} {size_str}   "
            f"{Fore.RED}WA wrong decompressed block{Style.RESET_ALL}"
        )
        return {
            "index": i,
            "test_file": test_file,
            "message": message,
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

    # Set up command line argument parsing
    parser = argparse.ArgumentParser(
        description="Run compress/decompress tests in parallel."
    )
    parser.add_argument(
        "--bin",
        default="../build/solution",
        help="Path to the solution binary (default: ../build/solution).",
    )
    args = parser.parse_args()

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
        (i + 1, test_file, name_width, n_tests, args.bin)
        for i, test_file in enumerate(test_files)
    ]

    # Run in parallel
    with Pool() as pool:
        results = pool.map(process_test, args_list)

    results.sort(key=lambda x: x["index"])

    n_ok = 0
    total_points = 0.0

    # Print the per-test results
    for res in results:
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
