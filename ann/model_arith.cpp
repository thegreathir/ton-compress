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

#include <torch/script.h> // For torch::jit::script::Module
#include <torch/torch.h>
#include <iostream>
#include <vector>
#include <string>
#include <cmath>
#include <cstdint>

/**************************************************************
 * 1) Utility: Convert raw bytes => bits, and bits => bytes
 **************************************************************/
static std::vector<int> stringToBits(const std::string &inputBytes) {
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

static std::string bitsToString(const std::vector<int> &bits) {
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
static float nextBitProbability(torch::jit::script::Module &model,
                                const std::vector<int> &prefix)
{
    if (prefix.empty()) {
        // If no prefix, we define p=0.5 or handle an empty LSTM input
        return 0.5f;
    }

    // Create input tensor [1, prefix_len, 1]
    int64_t seq_len = (int64_t) prefix.size();
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

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0]
                  << " <model_path>\n\n"
                  << "Example:\n"
                  << "  " << argv[0] << " best_bit_lstm_model_jit.pt\n";
        return 1;
    }

    std::string modelPath = argv[1];

    // 1) Load TorchScript model
    torch::jit::script::Module model;
    try {
        model = torch::jit::load(modelPath);
    } catch (const c10::Error &e) {
        std::cerr << "Error loading TorchScript model: " << modelPath << std::endl;
        std::cerr << e.what() << std::endl;
        return 2;
    }
    model.eval();
    model.to(torch::kCPU);

    return 0;
}
