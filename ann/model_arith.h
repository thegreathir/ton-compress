#ifndef MODEL_ARITH_H
#define MODEL_ARITH_H

#include "torch/script.h"
#include "torch/torch.h"
#include <cmath>
#include <deque>
#include <string>
#include <vector>

std::vector<int> stringToBits(const std::string &inputBytes);
std::string bitsToString(const std::vector<int> &bits);
float nextBitProbability(torch::jit::script::Module &model,
                         const std::deque<int> &prefix);
std::string compressBits(torch::jit::script::Module &model,
                         const std::vector<int> &bits);
std::vector<int> decompressBits(torch::jit::script::Module &model,
                                const std::string &data, int size);
torch::jit::script::Module loadModel(const std::string &modelPath);
#endif // MODEL_ARITH_H
