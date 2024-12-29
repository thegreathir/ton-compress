/*
 * solution.cpp
 *
 * Example solution.
 * This is (almost) how blocks are actually compressed in TON.
 * Normally, blocks are stored using vm::std_boc_serialize with mode=31.
 * Compression algorithm takes a block, converts it to mode=2 (which has less
 * extra information) and compresses it using lz4.
 */
#include <algorithm>
#include <iostream>
#include <queue>
#include <random>
#include <vector>

#include "td/utils/base64.h"
#include "td/utils/crypto.h"
#include "td/utils/lz4.h"
#include "td/utils/misc.h"
#include "vm/boc.h"

std::random_device rd;
std::mt19937 rng(rd());

const int POPULATION = 10;
const int CHILDREN = 5;
const int MUTATION = 5;

struct CellSerializationInfo {
  bool special;
  vm::Cell::LevelMask level_mask;

  bool with_hashes;
  size_t hashes_offset;
  size_t depth_offset;

  size_t data_offset;
  size_t data_len;
  bool data_with_bits;

  size_t refs_offset;
  int refs_cnt;

  size_t end_offset;

  td::Status init(td::Slice data, int ref_byte_size) {
    if (data.size() < 2) {
      return td::Status::Error(PSLICE() << "Not enough bytes "
                                        << td::tag("got", data.size())
                                        << td::tag("expected", "at least 2"));
    }
    TRY_STATUS(init(data.ubegin()[0], data.ubegin()[1], ref_byte_size));
    if (data.size() < end_offset) {
      return td::Status::Error(PSLICE() << "Not enough bytes "
                                        << td::tag("got", data.size())
                                        << td::tag("expected", end_offset));
    }
    return td::Status::OK();
  }

  td::Status init(td::uint8 d1, td::uint8 d2, int ref_byte_size) {
    refs_cnt = d1 & 7;
    level_mask = vm::Cell::LevelMask(d1 >> 5);
    special = (d1 & 8) != 0;
    with_hashes = (d1 & 16) != 0;

    if (refs_cnt > 4) {
      if (refs_cnt != 7 || !with_hashes) {
        return td::Status::Error("Invalid first byte");
      }
      refs_cnt = 0;
      // ...
      // do not deserialize absent cells!
      return td::Status::Error("TODO: absent cells");
    }

    hashes_offset = 2;
    auto n = level_mask.get_hashes_count();
    depth_offset = hashes_offset + (with_hashes ? n * vm::Cell::hash_bytes : 0);
    data_offset = depth_offset + (with_hashes ? n * vm::Cell::depth_bytes : 0);
    data_len = (d2 >> 1) + (d2 & 1);
    data_with_bits = (d2 & 1) != 0;
    refs_offset = data_offset + data_len;
    end_offset = refs_offset + refs_cnt * ref_byte_size;

    return td::Status::OK();
  }
  td::Result<int> get_bits(td::Slice cell) const {
    if (data_with_bits) {
      DCHECK(data_len != 0);
      int last = cell[data_offset + data_len - 1];
      if (!(last & 0x7f)) {
        return td::Status::Error("overlong encoding");
      }
      return td::narrow_cast<int>((data_len - 1) * 8 + 7 -
                                  td::count_trailing_zeroes_non_zero32(last));
    } else {
      return td::narrow_cast<int>(data_len * 8);
    }
  }

  td::Result<td::Ref<vm::DataCell>>
  create_data_cell(td::Slice cell_slice,
                   td::Span<td::Ref<vm::Cell>> refs) const {
    vm::CellBuilder cb;
    TRY_RESULT(bits, get_bits(cell_slice));
    cb.store_bits(cell_slice.ubegin() + data_offset, bits);
    DCHECK(refs_cnt == (td::int64)refs.size());
    for (int k = 0; k < refs_cnt; k++) {
      cb.store_ref(std::move(refs[k]));
    }
    TRY_RESULT(res, cb.finalize_novm_nothrow(special));
    CHECK(!res.is_null());
    if (res->is_special() != special) {
      return td::Status::Error("is_special mismatch");
    }
    if (res->get_level_mask() != level_mask) {
      return td::Status::Error("level mask mismatch");
    }
    // return res;
    if (with_hashes) {
      auto hash_n = level_mask.get_hashes_count();
      if (res->get_hash().as_slice() !=
          cell_slice.substr(hashes_offset + vm::Cell::hash_bytes * (hash_n - 1),
                            vm::Cell::hash_bytes)) {
        return td::Status::Error("representation hash mismatch");
      }
      if (res->get_depth() !=
          vm::DataCell::load_depth(
              cell_slice
                  .substr(depth_offset + vm::Cell::depth_bytes * (hash_n - 1),
                          vm::Cell::depth_bytes)
                  .ubegin())) {
        return td::Status::Error("depth mismatch");
      }

      bool check_all_hashes = true;
      for (unsigned level_i = 0, hash_i = 0, level = level_mask.get_level();
           check_all_hashes && level_i < level; level_i++) {
        if (!level_mask.is_significant(level_i)) {
          continue;
        }
        if (cell_slice.substr(hashes_offset + vm::Cell::hash_bytes * hash_i,
                              vm::Cell::hash_bytes) !=
            res->get_hash(level_i).as_slice()) {
          // hash mismatch
          return td::Status::Error("lower hash mismatch");
        }
        if (res->get_depth(level_i) !=
            vm::DataCell::load_depth(
                cell_slice
                    .substr(depth_offset + vm::Cell::depth_bytes * hash_i,
                            vm::Cell::depth_bytes)
                    .ubegin())) {
          return td::Status::Error("lower depth mismatch");
        }
        hash_i++;
      }
    }
    return res;
  }
};

class MyBagOfCells {
public:
  enum { hash_bytes = vm::Cell::hash_bytes, default_max_roots = 16384 };
  enum Mode {
    WithIndex = 1,
    WithCRC32C = 2,
    WithTopHash = 4,
    WithIntHashes = 8,
    WithCacheBits = 16,
    max = 31
  };
  enum { max_cell_whs = 64 };
  using Hash = vm::Cell::Hash;
  struct Info {
    enum : td::uint32 {
      boc_idx = 0x68ff65f3,
      boc_idx_crc32c = 0xacc3a728,
      boc_generic = 0xb5ee9c72
    };

    unsigned magic;
    int root_count;
    int cell_count;
    int absent_count;
    int ref_byte_size;
    int offset_byte_size;
    bool valid;
    bool has_index;
    bool has_roots{false};
    bool has_crc32c;
    bool has_cache_bits;
    unsigned long long roots_offset, index_offset, data_offset, data_size,
        total_size;
    Info() : magic(0), valid(false) {}
    void invalidate() { valid = false; }

    unsigned long long read_ref(const unsigned char *ptr) {
      return read_int(ptr, ref_byte_size);
    }
    unsigned long long read_offset(const unsigned char *ptr) {
      return read_int(ptr, offset_byte_size);
    }

    unsigned long long read_int(const unsigned char *ptr, unsigned bytes) {
      unsigned long long res = 0;
      while (bytes > 0) {
        res = (res << 8) + *ptr++;
        --bytes;
      }
      return res;
    }

    void write_int(unsigned char *ptr, unsigned long long value, int bytes) {
      ptr += bytes;
      while (bytes) {
        *--ptr = value & 0xff;
        value >>= 8;
        --bytes;
      }
      DCHECK(!bytes);
    }

    long long parse_serialized_header(const td::Slice &slice) {
      invalidate();
      int sz = static_cast<int>(
          std::min(slice.size(), static_cast<std::size_t>(0xffff)));
      if (sz < 4) {
        return -10; // want at least 10 bytes
      }
      const unsigned char *ptr = slice.ubegin();
      magic = (unsigned)read_int(ptr, 4);
      has_crc32c = false;
      has_index = false;
      has_cache_bits = false;
      ref_byte_size = 0;
      offset_byte_size = 0;
      root_count = cell_count = absent_count = -1;
      index_offset = data_offset = data_size = total_size = 0;
      if (magic != boc_generic && magic != boc_idx && magic != boc_idx_crc32c) {
        magic = 0;
        return 0;
      }
      if (sz < 5) {
        return -10;
      }
      td::uint8 byte = ptr[4];
      if (magic == boc_generic) {
        has_index = (byte >> 7) % 2 == 1;
        has_crc32c = (byte >> 6) % 2 == 1;
        has_cache_bits = (byte >> 5) % 2 == 1;
      } else {
        has_index = true;
        has_crc32c = magic == boc_idx_crc32c;
      }
      if (has_cache_bits && !has_index) {
        return 0;
      }
      ref_byte_size = byte & 7;
      if (ref_byte_size > 4 || ref_byte_size < 1) {
        return 0;
      }
      if (sz < 6) {
        return -7 - 3 * ref_byte_size;
      }
      offset_byte_size = ptr[5];
      if (offset_byte_size > 8 || offset_byte_size < 1) {
        return 0;
      }
      roots_offset = 6 + 3 * ref_byte_size + offset_byte_size;
      ptr += 6;
      sz -= 6;
      if (sz < ref_byte_size) {
        return -static_cast<int>(roots_offset);
      }
      cell_count = (int)read_ref(ptr);
      if (cell_count <= 0) {
        cell_count = -1;
        return 0;
      }
      if (sz < 2 * ref_byte_size) {
        return -static_cast<int>(roots_offset);
      }
      root_count = (int)read_ref(ptr + ref_byte_size);
      if (root_count <= 0) {
        root_count = -1;
        return 0;
      }
      index_offset = roots_offset;
      if (magic == boc_generic) {
        index_offset += (long long)root_count * ref_byte_size;
        has_roots = true;
      } else {
        if (root_count != 1) {
          return 0;
        }
      }
      data_offset = index_offset;
      if (has_index) {
        data_offset += (long long)cell_count * offset_byte_size;
      }
      if (sz < 3 * ref_byte_size) {
        return -static_cast<int>(roots_offset);
      }
      absent_count = (int)read_ref(ptr + 2 * ref_byte_size);
      if (absent_count < 0 || absent_count > cell_count) {
        return 0;
      }
      if (sz < 3 * ref_byte_size + offset_byte_size) {
        return -static_cast<int>(roots_offset);
      }
      data_size = read_offset(ptr + 3 * ref_byte_size);
      if (data_size > ((unsigned long long)cell_count << 10)) {
        return 0;
      }
      if (data_size > (1ull << 40)) {
        return 0; // bag of cells with more than 1TiB data is unlikely
      }
      if (data_size < cell_count * (2ull + ref_byte_size) - ref_byte_size) {
        return 0; // invalid header, too many cells for this amount of data
                  // bytes
      }
      valid = true;
      total_size = data_offset + data_size + (has_crc32c ? 4 : 0);
      return total_size;
    }
  };

public:
  int cell_count{0}, root_count{0}, dangle_count{0}, int_refs{0};
  int int_hashes{0}, top_hashes{0};
  int max_depth{1024};
  Info info;
  unsigned long long data_bytes{0};
  td::HashMap<Hash, int> cells;
  struct CellInfo {
    td::Ref<vm::DataCell> dc_ref;
    std::array<int, 4> ref_idx;
    unsigned char ref_num;
    unsigned char wt;
    unsigned char hcnt;
    int new_idx;
    bool should_cache{false};
    bool is_root_cell{false};
    CellInfo() : ref_num(0) {}
    CellInfo(td::Ref<vm::DataCell> _dc) : dc_ref(std::move(_dc)), ref_num(0) {}
    CellInfo(td::Ref<vm::DataCell> _dc, int _refs,
             const std::array<int, 4> &_ref_list)
        : dc_ref(std::move(_dc)), ref_idx(_ref_list),
          ref_num(static_cast<unsigned char>(_refs)) {}
    bool is_special() const { return !wt; }
  };
  std::vector<CellInfo> cell_list_;
  struct RootInfo {
    RootInfo() = default;
    RootInfo(td::Ref<vm::Cell> cell, int idx)
        : cell(std::move(cell)), idx(idx) {}
    td::Ref<vm::Cell> cell;
    int idx{-1};
  };
  std::vector<CellInfo> cell_list_tmp;
  std::vector<RootInfo> roots;
  std::vector<unsigned char> serialized;
  const unsigned char *index_ptr{nullptr};
  const unsigned char *data_ptr{nullptr};
  std::vector<unsigned long long> custom_index;

public:
  MyBagOfCells() = default;
  int get_root_count() const { return root_count; }

  void clear() {
    cells_clear();
    roots.clear();
    root_count = 0;
    serialized.clear();
  }

  bool get_cache_entry(int index) {
    if (!info.has_cache_bits) {
      return true;
    }
    if (!info.has_index) {
      return true;
    }
    auto raw = get_idx_entry_raw(index);
    return raw % 2 == 1;
  }

  unsigned long long get_idx_entry_raw(int index) {
    if (index < 0) {
      return 0;
    }
    if (!info.has_index) {
      return custom_index.at(index);
    } else if (index < info.cell_count && index_ptr) {
      return info.read_offset(index_ptr + (long)index * info.offset_byte_size);
    } else {
      // throw ?
      return 0;
    }
  }

  unsigned long long get_idx_entry(int index) {
    auto raw = get_idx_entry_raw(index);
    if (info.has_cache_bits) {
      raw /= 2;
    }
    return raw;
  }

  td::Result<td::Slice> get_cell_slice(int idx, td::Slice data) {
    unsigned long long offs = get_idx_entry(idx - 1);
    unsigned long long offs_end = get_idx_entry(idx);
    if (offs > offs_end || offs_end > data.size()) {
      return td::Status::Error(PSLICE() << "invalid index entry [" << offs
                                        << "; " << offs_end << "], "
                                        << td::tag("data.size()", data.size()));
    }
    return data.substr(offs, td::narrow_cast<size_t>(offs_end - offs));
  }

  td::Result<std::vector<int>> get_topol_order(td::Slice cells_slice,
                                               int cell_count) {
    std::vector<std::vector<int>> rev_graph(cell_count);
    std::vector<int> in_degree(cell_count, 0);

    for (int idx = 0; idx < cell_count; idx++) {
      TRY_RESULT(cell_slice, get_cell_slice(idx, cells_slice));

      CellSerializationInfo cell_info;
      TRY_STATUS(cell_info.init(cell_slice, info.ref_byte_size));
      if (cell_info.end_offset != cell_slice.size()) {
        return td::Status::Error("unused space in cell serialization");
      }

      for (int k = 0; k < cell_info.refs_cnt; k++) {
        int ref_idx =
            (int)info.read_ref(cell_slice.ubegin() + cell_info.refs_offset +
                               k * info.ref_byte_size);

        rev_graph[ref_idx].push_back(idx);
        in_degree[idx]++;
      }
    }

    std::vector<int> topol;
    std::queue<int> q;
    for (int idx = 0; idx < cell_count; idx++) {
      if (!in_degree[idx]) {
        q.push(idx);
      }
    }

    while (!q.empty()) {
      int idx = q.front();
      q.pop();
      topol.push_back(idx);

      for (int ref_idx : rev_graph[idx]) {
        in_degree[ref_idx]--;
        if (!in_degree[ref_idx]) {
          q.push(ref_idx);
        }
      }
    }

    return topol;
  }

  td::Result<td::Ref<vm::DataCell>>
  deserialize_cell(std::vector<int> idx_map, int idx, td::Slice cells_slice,
                   td::Span<td::Ref<vm::DataCell>> cells_span,
                   std::vector<td::uint8> *cell_should_cache) {
    TRY_RESULT(cell_slice, get_cell_slice(idx, cells_slice));
    std::array<td::Ref<vm::Cell>, 4> refs_buf;

    CellSerializationInfo cell_info;
    TRY_STATUS(cell_info.init(cell_slice, info.ref_byte_size));
    if (cell_info.end_offset != cell_slice.size()) {
      return td::Status::Error("unused space in cell serialization");
    }

    auto refs = td::MutableSpan<td::Ref<vm::Cell>>(refs_buf).substr(
        0, cell_info.refs_cnt);
    for (int k = 0; k < cell_info.refs_cnt; k++) {
      int ref_idx = (int)info.read_ref(
          cell_slice.ubegin() + cell_info.refs_offset + k * info.ref_byte_size);
      if (idx_map[ref_idx] == -1) {
        return td::Status::Error(
            PSLICE() << "bag-of-cells error: reference #" << k << " of cell #"
                     << idx << " is to cell #" << ref_idx
                     << " which does not appear earlier in topological order");
      }
      if (ref_idx >= cell_count) {
        return td::Status::Error(
            PSLICE() << "bag-of-cells error: reference #" << k << " of cell #"
                     << idx << " is to non-existent cell #" << ref_idx
                     << ", only " << cell_count << " cells are defined");
      }
      refs[k] = cells_span[idx_map[ref_idx]];
      if (cell_should_cache) {
        auto &cnt = (*cell_should_cache)[ref_idx];
        if (cnt < 2) {
          cnt++;
        }
      }
    }

    return cell_info.create_data_cell(cell_slice, refs);
  }

  td::Result<long long> deserialize(const td::Slice &data, int max_roots) {
    clear();
    long long size_est = info.parse_serialized_header(data);
    // LOG(INFO) << "estimated size " << size_est << ", true size " <<
    // data.size();
    if (size_est == 0) {
      return td::Status::Error(
          PSLICE() << "cannot deserialize bag-of-cells: invalid header, error "
                   << size_est);
    }
    if (size_est < 0) {
      // LOG(ERROR) << "cannot deserialize bag-of-cells: not enough bytes (" <<
      // data.size() << " present, " << -size_est
      //<< " required)";
      return size_est;
    }

    if (size_est > (long long)data.size()) {
      // LOG(ERROR) << "cannot deserialize bag-of-cells: not enough bytes (" <<
      // data.size() << " present, " << size_est
      //<< " required)";
      return -size_est;
    }
    // LOG(INFO) << "estimated size " << size_est << ", true size " <<
    // data.size();
    if (info.root_count > max_roots) {
      return td::Status::Error(
          "Bag-of-cells has more root cells than expected");
    }
    if (info.has_crc32c) {
      unsigned crc_computed =
          td::crc32c(td::Slice{data.ubegin(), data.uend() - 4});
      unsigned crc_stored = td::as<unsigned>(data.uend() - 4);
      if (crc_computed != crc_stored) {
        return td::Status::Error(
            PSLICE() << "bag-of-cells CRC32C mismatch: expected "
                     << td::format::as_hex(crc_computed) << ", found "
                     << td::format::as_hex(crc_stored));
      }
    }

    cell_count = info.cell_count;
    std::vector<td::uint8> cell_should_cache;
    if (info.has_cache_bits) {
      cell_should_cache.resize(cell_count, 0);
    }
    roots.clear();
    roots.resize(info.root_count);
    auto *roots_ptr = data.substr(info.roots_offset).ubegin();
    for (int i = 0; i < info.root_count; i++) {
      int idx = 0;
      if (info.has_roots) {
        idx = (int)info.read_ref(roots_ptr + i * info.ref_byte_size);
      }
      if (idx < 0 || idx >= info.cell_count) {
        return td::Status::Error(PSLICE()
                                 << "bag-of-cells invalid root index " << idx);
      }
      roots[i].idx = idx;
      if (info.has_cache_bits) {
        auto &cnt = cell_should_cache[idx];
        if (cnt < 2) {
          cnt++;
        }
      }
    }
    if (info.has_index) {
      index_ptr = data.substr(info.index_offset).ubegin();
      // TODO: should we validate index here
    } else {
      index_ptr = nullptr;
      unsigned long long cur = 0;
      custom_index.reserve(info.cell_count);

      auto cells_slice = data.substr(info.data_offset, info.data_size);

      for (int i = 0; i < info.cell_count; i++) {
        CellSerializationInfo cell_info;
        auto status = cell_info.init(cells_slice, info.ref_byte_size);
        if (status.is_error()) {
          return td::Status::Error(
              PSLICE() << "invalid bag-of-cells failed to deserialize cell #"
                       << i << " " << status.error());
        }
        cells_slice = cells_slice.substr(cell_info.end_offset);
        cur += cell_info.end_offset;
        custom_index.push_back(cur);
      }
      if (!cells_slice.empty()) {
        return td::Status::Error(PSLICE()
                                 << "invalid bag-of-cells last cell #"
                                 << info.cell_count - 1 << ": end offset "
                                 << cur << " is different from total data size "
                                 << info.data_size);
      }
    }
    auto cells_slice = data.substr(info.data_offset, info.data_size);
    std::vector<td::Ref<vm::DataCell>> cell_list;
    cell_list.reserve(cell_count);
    std::array<td::Ref<vm::Cell>, 4> refs_buf;

    auto topol_order = get_topol_order(cells_slice, cell_count).move_as_ok();
    std::vector<int> idx_map(cell_count, -1);

    for (auto &&idx : topol_order) {
      auto r_cell =
          deserialize_cell(idx_map, idx, cells_slice, cell_list,
                           info.has_cache_bits ? &cell_should_cache : nullptr);
      if (r_cell.is_error()) {
        return td::Status::Error(
            PSLICE() << "invalid bag-of-cells failed to deserialize cell #"
                     << idx << " " << r_cell.error());
      }
      idx_map[idx] = cell_list.size();
      cell_list.push_back(r_cell.move_as_ok());
      DCHECK(cell_list.back().not_null());
    }

    if (info.has_cache_bits) {
      for (int idx = 0; idx < cell_count; idx++) {
        auto should_cache = cell_should_cache[idx] > 1;
        auto stored_should_cache = get_cache_entry(idx);
        if (should_cache != stored_should_cache) {
          return td::Status::Error(PSLICE() << "invalid bag-of-cells cell #"
                                            << idx << " has wrong cache flag "
                                            << stored_should_cache);
        }
      }
    }
    custom_index.clear();
    index_ptr = nullptr;
    root_count = info.root_count;
    dangle_count = info.absent_count;
    for (auto &root_info : roots) {
      root_info.cell = cell_list[idx_map[root_info.idx]];
    }
    cell_list.clear();
    return size_est;
  }

  td::Ref<vm::Cell> get_root_cell(int idx = 0) const {
    return (idx >= 0 && idx < root_count) ? roots.at(idx).cell
                                          : td::Ref<vm::Cell>{};
  }
  void permute(std::vector<bool> mask) {
    std::vector<int> perm(cell_count);
    for (int i = 0; i < cell_count; i++) {
      perm[i] = i;
    }
    // int start = 0; // rng() % cell_count;
    for (int i = 0; i < cell_count - 1; i++) {
      // int end = std::min(start + 5, cell_count);
      if (!mask.empty() && mask[i]) {
        std::swap(perm[i], perm[i + 1]);
        // std::reverse(perm.begin() + start, perm.begin() + end);
      }
      // start = end;
    }
    // if(rng() % 2 == 0) {
    // std::reverse(perm.begin(), perm.end());
    // }
    for (int i = 0; i < cell_count; i++) {
      cell_list_[i].new_idx = perm[i];
    }

    for (int i = 0; i < cell_count; i++) {
      for (int j = 0; j < cell_list_[i].ref_num; j++) {
        cell_list_[i].ref_idx[j] = perm[cell_list_[i].ref_idx[j]];
      }
    }

    for (int i = 0; i < root_count; i++) {
      roots[i].idx = perm[roots[i].idx];
    }
    std::sort(cell_list_.begin(), cell_list_.end(),
              [](const CellInfo &a, const CellInfo &b) {
                return a.new_idx < b.new_idx;
              });
    for (int i = 0; i < cell_count; i++) {
      cells[cell_list_[i].dc_ref->get_hash()] = i;
    }
  }

private:
  int rv_idx;
  void cells_clear() {
    cell_count = 0;
    int_refs = 0;
    data_bytes = 0;
    cells.clear();
    cell_list_.clear();
  }
};

int number_of_cells = -1;

td::Result<td::BufferSlice> my_std_boc_serialize(std::vector<bool> mask,
                                                 td::Ref<vm::Cell> root,
                                                 int mode = 0) {
  if (root.is_null()) {
    return td::Status::Error(
        "cannot serialize a null cell reference into a bag of cells");
  }
  vm::BagOfCells boc;
  boc.add_root(std::move(root));
  auto res = boc.import_cells();

  auto myBoc = reinterpret_cast<MyBagOfCells *>(&boc);
  myBoc->permute(mask);
  number_of_cells = myBoc->cell_count;

  if (res.is_error()) {
    return res.move_as_error();
  }
  return boc.serialize_to_slice(mode);
}

td::Result<td::Ref<vm::Cell>>
my_std_boc_deserialize(td::Slice data, bool can_be_empty = false,
                       bool allow_nonzero_level = false) {
  if (data.empty() && can_be_empty) {
    return td::Ref<vm::Cell>();
  }
  vm::BagOfCells boc;
  auto myBoc = reinterpret_cast<MyBagOfCells *>(&boc);
  auto res = myBoc->deserialize(data, 1);
  if (res.is_error()) {
    return res.move_as_error();
  }
  if (boc.get_root_count() != 1) {
    return td::Status::Error(
        "bag of cells is expected to have exactly one root");
  }
  auto root = boc.get_root_cell();
  if (root.is_null()) {
    return td::Status::Error("bag of cells has null root cell (?)");
  }
  if (!allow_nonzero_level && root->get_level() != 0) {
    return td::Status::Error("bag of cells has a root with non-zero level");
  }
  return std::move(root);
}

struct Gene {
public:
  std::vector<bool> mask;
  int unfitness;

  Gene(std::vector<bool> mask, int unfitness)
      : mask(mask), unfitness(unfitness) {}
  void mutate(int cnt = 1) {
    while (cnt--) {
      int idx = rng() % mask.size();
      mask[idx] = !mask[idx];
    }
  }
};

bool operator<(const Gene &a, const Gene &b) {
  return a.unfitness < b.unfitness;
}
Gene merge(const Gene &a, const Gene &b) {
  std::vector<bool> mask;
  for (int i = 0; i < a.mask.size(); i++) {
    mask.push_back(rng() % 2 == 0 ? a.mask[i] : b.mask[i]);
  }
  return Gene(mask, 0);
}

td::BufferSlice compress(td::Slice data) {
  auto start_time_ms = clock() * 1000 / CLOCKS_PER_SEC;

  td::Ref<vm::Cell> root = vm::std_boc_deserialize(data).move_as_ok();

  td::BufferSlice best =
      td::lz4_compress(my_std_boc_serialize({}, root, 2).move_as_ok());
  int normal_len = best.length();
  int attempts = 0;

  auto generateGene = [&]() {
    std::vector<bool> mask;
    for (int i = 0; i < number_of_cells; i++) {
      mask.push_back(rng() % 2 == 0);
    }
    return Gene(mask, 1e9);
  };
  auto evalGene = [&](Gene &gene) {
    auto ser = my_std_boc_serialize(gene.mask, root, 2).move_as_ok();
    auto compressed = td::lz4_compress(ser);
    gene.unfitness = compressed.length();

    if (compressed.length() < best.length()) {
      best = std::move(compressed);
    }
  };

  std::vector<Gene> population;
  for (int i = 0; i < POPULATION; i++) {
    population.push_back(generateGene());
    evalGene(population.back());
  }
  std::sort(population.begin(), population.end());

  while (true) {
    attempts++;

    std::vector<Gene> childs, new_population;
    for (int i = 0; i < CHILDREN; i++) {
      int par1 = rng() % population.size();
      int par2 = rng() % population.size();
      auto child = merge(population[par1], population[par2]);
      child.mutate(rng() % MUTATION);
      evalGene(child);
      childs.push_back(child);
    }

    std::sort(childs.begin(), childs.end());
    int it1 = 0;
    int it2 = 0;
    while (new_population.size() < population.size()) {
      if (it1 < population.size() && it2 < childs.size()) {
        if (population[it1].unfitness < childs[it2].unfitness) {
          new_population.push_back(population[it1]);
          it1++;
        } else {
          new_population.push_back(childs[it2]);
          it2++;
        }
      } else if (it1 < population.size()) {
        new_population.push_back(population[it1]);
        it1++;
      } else {
        new_population.push_back(childs[it2]);
        it2++;
      }
    }

    population = std::move(new_population);

    auto elapsed_time_ms = clock() * 1000 / CLOCKS_PER_SEC - start_time_ms;
    // std::cerr << elapsed_time_ms << ", " << attempts << ", "
    // << population[0].unfitness << "\n";

    if (elapsed_time_ms > 1900)
      break;
  }

  // std::cerr << "best len: " << best.length() << "\n";
  // std::cerr << "normal len: " << normal_len << "\n";
  // std::cerr << "attempts: " << attempts << "\n";
  return best;
}

td::BufferSlice decompress(td::Slice data) {
  td::BufferSlice serialized = td::lz4_decompress(data, 2 << 20).move_as_ok();
  auto root = my_std_boc_deserialize(serialized).move_as_ok();
  return vm::std_boc_serialize(root, 31).move_as_ok();
}

int main() {
  std::string mode;
  std::cin >> mode;
  CHECK(mode == "compress" || mode == "decompress");

  std::string base64_data;
  std::cin >> base64_data;
  CHECK(!base64_data.empty());

  td::BufferSlice data(td::base64_decode(base64_data).move_as_ok());

  if (mode == "compress") {
    data = compress(data);
  } else {
    data = decompress(data);
  }

  std::cout << td::base64_encode(data) << std::endl;
}
