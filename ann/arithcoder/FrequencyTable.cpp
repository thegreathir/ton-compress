/*
 * Reference arithmetic coding
 *
 * Copyright (c) Project Nayuki
 * MIT License. See readme file.
 * https://www.nayuki.io/page/reference-arithmetic-coding
 */

#include "FrequencyTable.hpp"
#include <cassert>
#include <stdexcept>

using std::uint32_t;

FrequencyTable::~FrequencyTable() {}

BinaryFrequencyTable::BinaryFrequencyTable() : p(TOTAL / 2) {}

uint32_t BinaryFrequencyTable::getSymbolLimit() const { return 2; }

uint32_t BinaryFrequencyTable::get(uint32_t symbol) const {
  checkSymbol(symbol);
  return 1;
}

uint32_t BinaryFrequencyTable::getTotal() const {
  return BinaryFrequencyTable::TOTAL;
}

uint32_t BinaryFrequencyTable::getLow(uint32_t symbol) const {
  checkSymbol(symbol);
  if (!symbol)
    return 0;
  return p;
}

uint32_t BinaryFrequencyTable::getHigh(uint32_t symbol) const {
  checkSymbol(symbol);
  if (symbol)
    return BinaryFrequencyTable::TOTAL;
  return p;
}

void BinaryFrequencyTable::set(std::uint32_t symbol, std::uint32_t freq) {
  assert(!symbol);
  assert(freq <= BinaryFrequencyTable::TOTAL);
  p = freq;
  if (!p)
    p++;
  if (p == BinaryFrequencyTable::TOTAL)
    p--;
}

void BinaryFrequencyTable::set(float prob) {
  long double ld_prob = prob;
  std::uint32_t freq = (std::uint32_t)(ld_prob * BinaryFrequencyTable::TOTAL);
  set(0, freq);
}

void BinaryFrequencyTable::increment(uint32_t) {
  throw std::logic_error("Unsupported operation");
}

void BinaryFrequencyTable::checkSymbol(uint32_t symbol) const {
  if (symbol >= 2)
    throw std::domain_error("Symbol out of range");
}

FlatFrequencyTable::FlatFrequencyTable(uint32_t numSyms) : numSymbols(numSyms) {
  if (numSyms < 1)
    throw std::domain_error("Number of symbols must be positive");
}

uint32_t FlatFrequencyTable::getSymbolLimit() const { return numSymbols; }

uint32_t FlatFrequencyTable::get(uint32_t symbol) const {
  checkSymbol(symbol);
  return 1;
}

uint32_t FlatFrequencyTable::getTotal() const { return numSymbols; }

uint32_t FlatFrequencyTable::getLow(uint32_t symbol) const {
  checkSymbol(symbol);
  return symbol;
}

uint32_t FlatFrequencyTable::getHigh(uint32_t symbol) const {
  checkSymbol(symbol);
  return symbol + 1;
}

void FlatFrequencyTable::set(uint32_t, uint32_t) {
  throw std::logic_error("Unsupported operation");
}

void FlatFrequencyTable::increment(uint32_t) {
  throw std::logic_error("Unsupported operation");
}

void FlatFrequencyTable::checkSymbol(uint32_t symbol) const {
  if (symbol >= numSymbols)
    throw std::domain_error("Symbol out of range");
}

SimpleFrequencyTable::SimpleFrequencyTable(const std::vector<uint32_t> &freqs) {
  if (freqs.size() > UINT32_MAX - 1)
    throw std::length_error("Too many symbols");
  uint32_t size = static_cast<uint32_t>(freqs.size());
  if (size < 1)
    throw std::invalid_argument("At least 1 symbol needed");

  frequencies = freqs;
  cumulative.reserve(size + 1);
  initCumulative(false);
  total = getHigh(size - 1);
}

SimpleFrequencyTable::SimpleFrequencyTable(const FrequencyTable &freqs) {
  uint32_t size = freqs.getSymbolLimit();
  if (size < 1)
    throw std::invalid_argument("At least 1 symbol needed");
  if (size > UINT32_MAX - 1)
    throw std::length_error("Too many symbols");

  frequencies.reserve(size + 1);
  for (uint32_t i = 0; i < size; i++)
    frequencies.push_back(freqs.get(i));

  cumulative.reserve(size + 1);
  initCumulative(false);
  total = getHigh(size - 1);
}

uint32_t SimpleFrequencyTable::getSymbolLimit() const {
  return static_cast<uint32_t>(frequencies.size());
}

uint32_t SimpleFrequencyTable::get(uint32_t symbol) const {
  return frequencies.at(symbol);
}

void SimpleFrequencyTable::set(uint32_t symbol, uint32_t freq) {
  if (total < frequencies.at(symbol))
    throw std::logic_error("Assertion error");
  uint32_t temp = total - frequencies.at(symbol);
  total = checkedAdd(temp, freq);
  frequencies.at(symbol) = freq;
  cumulative.clear();
}

void SimpleFrequencyTable::increment(uint32_t symbol) {
  if (frequencies.at(symbol) == UINT32_MAX)
    throw std::overflow_error("Arithmetic overflow");
  total = checkedAdd(total, 1);
  frequencies.at(symbol)++;
  cumulative.clear();
}

uint32_t SimpleFrequencyTable::getTotal() const { return total; }

uint32_t SimpleFrequencyTable::getLow(uint32_t symbol) const {
  initCumulative();
  return cumulative.at(symbol);
}

uint32_t SimpleFrequencyTable::getHigh(uint32_t symbol) const {
  initCumulative();
  return cumulative.at(symbol + 1);
}

void SimpleFrequencyTable::initCumulative(bool checkTotal) const {
  if (!cumulative.empty())
    return;
  uint32_t sum = 0;
  cumulative.push_back(sum);
  for (uint32_t freq : frequencies) {
    // This arithmetic should not throw an exception, because invariants are
    // being maintained elsewhere in the data structure. This implementation is
    // just a defensive measure.
    sum = checkedAdd(freq, sum);
    cumulative.push_back(sum);
  }
  if (checkTotal && sum != total)
    throw std::logic_error("Assertion error");
}

uint32_t SimpleFrequencyTable::checkedAdd(uint32_t x, uint32_t y) {
  if (x > UINT32_MAX - y)
    throw std::overflow_error("Arithmetic overflow");
  return x + y;
}
