/**************************************************************
 * model_arith_1024.cpp
 *
 * Demonstrates:
 *   1) Loading a TorchScript LSTM model for bit prediction.
 *   2) A function to get P(bit=1) given a prefix of bits.
 *   3) Integer-based arithmetic coding (with partial renormalization)
 *      to compress and decompress up to 1024 bits (losslessly).
 *
 * Usage:
 *   g++ model_arith_1024.cpp -I/path/to/libtorch/include \
 *       -I/path/to/libtorch/include/torch/csrc/api/include \
 *       -L/path/to/libtorch/lib \
 *       -ltorch -ltorch_cpu -lc10 \
 *       -o model_arith_1024_app -std=c++17
 *
 *   ./model_arith_1024_app best_bit_lstm_model_jit.pt "HelloWorld"
 *
 * (Adjust include/library paths as needed for your system.)
 **************************************************************/

#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <torch/script.h> // For torch::jit::script::Module
#include <torch/torch.h>
#include <vector>

#include "arithcoder/ArithmeticCoder.hpp"
#include "model_arith.h"
#include "td/utils/base64.h"

const int CONTEXT_SIZE = 192;
#define PAD

/**************************************************************
 * 1) Utility: Convert raw bytes => bits, and bits => bytes
 **************************************************************/
std::vector<int> stringToBits(const std::string &inputBytes) {
  std::vector<int> bits;
  bits.reserve(inputBytes.size() * 8);
  for (unsigned char c : inputBytes) {
    // Extract bits from MSB to LSB
    for (int i = 7; i >= 0; --i) {
      int bit = (c >> i) & 1;
      bits.push_back(bit);
    }
  }
  return bits;
}

std::string bitsToString(const std::vector<int> &bits) {
  // Group bits into bytes
  const size_t nBytes = bits.size() / 8;
  std::string output;
  output.resize(nBytes, '\0');

  size_t idx = 0;
  for (size_t b = 0; b < nBytes; ++b) {
    unsigned char byteVal = 0;
    for (int i = 0; i < 8; ++i) {
      byteVal = (byteVal << 1) | (bits[idx++] & 1);
    }
    output[b] = static_cast<char>(byteVal);
  }
  return output;
}

/*********************************************************************
 * 2) nextBitProbability:
 *    Given a prefix of bits, query the LSTM to get P(next_bit=1).
 *
 *    - We build a tensor [1, prefix_len, 1].
 *    - Run forward.
 *    - Take the final time-step's logit => apply sigmoid => probability.
 *    - If prefix empty, default to 0.5
 *********************************************************************/
float nextBitProbability(torch::jit::script::Module &model,
                         const std::deque<int> &prefix) {
  if (prefix.empty()) {
    // If no prefix, we define p=0.5 or handle an empty LSTM input
    return 0.5f;
  }

  // Create input tensor [1, prefix_len, 1]
  int64_t seq_len = (int64_t)prefix.size();
  torch::Tensor inputTensor = torch::zeros({1, seq_len, 1}, torch::kFloat32);
  for (int64_t t = 0; t < seq_len; ++t) {
    inputTensor[0][t][0] = (float)prefix[t];
  }

  // Forward pass
  torch::Tensor output = model.forward({inputTensor}).toTensor();
  // output shape = [1, seq_len, 1]

  // Take final time-step logit
  float logit = output[0][seq_len - 1][0].item<float>();

  // Probability = sigmoid(logit)
  float p = 1.0f / (1.0f + std::exp(-logit));
  return p;
}

std::string compressBits(torch::jit::script::Module &model,
                         const std::vector<int> &bits) {
  std::string out;
  std::stringstream sout;
  BitOutputStream bout(sout);

  try {
    BinaryFrequencyTable freqs;
    ArithmeticEncoder enc(32, bout);

    std::deque<int> prefix;
#ifdef PAD
    for (int i = 0; i < CONTEXT_SIZE; i++) {
      prefix.push_back(0);
    }
#endif

    for (auto symbol : bits) {
      float p = nextBitProbability(model, prefix);
      freqs.set(1 - p);
      enc.write(freqs, static_cast<uint32_t>(symbol));
      prefix.push_back(symbol);
      if (prefix.size() > CONTEXT_SIZE) {
        prefix.pop_front();
      }
    }

    enc.finish(); // Flush remaining code bits
    bout.finish();

    out = sout.str();

  } catch (const char *msg) {
    std::cerr << msg << std::endl;
    assert(false);
  }

  return out;
}

std::vector<int> decompressBits(torch::jit::script::Module &model,
                                const std::string &data, int size) {
  std::vector<int> out;
  std::stringstream sin(data);
  BitInputStream bin(sin);

  try {
    BinaryFrequencyTable freqs;
    ArithmeticDecoder dec(32, bin);

    std::deque<int> prefix;
#ifdef PAD
    for (int i = 0; i < CONTEXT_SIZE; i++) {
      prefix.push_back(0);
    }
#endif

    while (true) {
      float p = nextBitProbability(model, prefix);
      freqs.set(1 - p);

      uint32_t symbol = dec.read(freqs);
      out.push_back(symbol);
      if (out.size() == size)
        break;

      prefix.push_back(symbol);
      if (prefix.size() > CONTEXT_SIZE) {
        prefix.pop_front();
      }
    }

  } catch (const char *msg) {
    std::cerr << msg << std::endl;
    assert(false);
  }

  return out;
}

torch::jit::script::Module loadModel(const std::string &modelPath) {
  torch::jit::script::Module model;
  try {
    model = torch::jit::load(modelPath);
  } catch (const c10::Error &e) {
    std::cerr << "Error loading TorchScript model: " << modelPath << std::endl;
    std::cerr << e.what() << std::endl;
    assert(false);
  }
  model.eval();
  model.to(torch::kCPU);
  return model;
}

int main(int argc, char **argv) {
  if (argc < 2) {
    std::cerr << "Usage: " << argv[0] << " <model_path>\n\n"
              << "Example:\n"
              << "  " << argv[0] << " best_bit_lstm_model_jit.pt\n";
    return 1;
  }

  std::string modelPath = argv[1];

  // 1) Load TorchScript model
  auto model = loadModel(modelPath);

  std::string base64_data;

  std::vector<int> init_sizes, comp_sizes;

  auto start_time = std::chrono::high_resolution_clock::now();

  int cnt = 0;
  while (std::getline(std::cin, base64_data)) {
    if (base64_data.empty())
      continue;
    std::string data = td::base64_decode(base64_data).move_as_ok();

    // auto bits = stringToBits(data);
    // auto res_bits =
    //     decompressBits(model, compressBits(model, bits), bits.size());
    // for (int i = 0; i < bits.size(); i++)
    //   if (bits[i] != res_bits[i]) {
    //     std::cerr << "KIR KHAR" << std::endl;
    //     return 1;
    //   }
    // std::cerr << "OK\n";

    std::string res =
        td::base64_encode(compressBits(model, stringToBits(data)));
    // std::cout << base64_data << " -> " << res << std::endl;
    std::cout << "number: " << (++cnt) << " ";
    std::cout << base64_data.size() << " -> " << res.size() << std::endl;
    init_sizes.push_back(base64_data.size());
    comp_sizes.push_back(res.size());
    // std::cout << res << std::endl;
  }

  long long x_int = 0, y_int = 0, yp_int = 0;

  for (int i = 0; i < init_sizes.size(); i++) {
    x_int += init_sizes[i];
    y_int += comp_sizes[i];
    yp_int += std::min(init_sizes[i], comp_sizes[i]);
  }

  long double x = x_int, y = y_int, yp = yp_int;

  long double score = 2 * x / (x + y);
  long double red = y / x;
  long double score_ed = 2 * x / (x + yp);
  long double red_ed = yp / x;

  std::cout << "Score: " << score << std::endl;
  std::cout << "Reduction: " << red << std::endl;
  std::cout << "Score_ed: " << score_ed << std::endl;
  std::cout << "Reduction_ed: " << red_ed << std::endl;
  std::cout << "Time in seconds: "
            << std::chrono::duration<double>(
                   std::chrono::high_resolution_clock::now() - start_time)
                   .count()
            << std::endl;
  return 0;
}
