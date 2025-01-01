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
#include <vector>

#include "td/utils/base64.h"
#include "td/utils/crypto.h"
#include "td/utils/lz4.h"
#include "td/utils/misc.h"
#include "vm/boc-writers.h"
#include "vm/boc.h"

class MyDataCell : public vm::Cell {
public:
  // NB: cells created with use_arena=true are never freed
  static thread_local bool use_arena;

  MyDataCell(const MyDataCell &other) = delete;
  ~MyDataCell() override;

  static void store_depth(td::uint8 *dest, td::uint16 depth) {
    td::bitstring::bits_store_long(dest, depth, depth_bits);
  }
  static td::uint16 load_depth(const td::uint8 *src) {
    return td::bitstring::bits_load_ulong(src, depth_bits) & 0xffff;
  }

public:
  struct Info {
    unsigned bits_;

    // d1
    unsigned char refs_count_ : 3;
    bool is_special_ : 1;
    unsigned char level_mask_ : 3;

    unsigned char hash_count_ : 3;

    unsigned char virtualization_ : 3;

    unsigned char d1() const { return d1(LevelMask{level_mask_}); }
    unsigned char d1(LevelMask level_mask) const {
      // d1 = refs_count + 8 * is_special + 32 * level
      //      + 16 * with_hashes - for seriazlization
      // d1 = 7 + 16 + 32 * l - for absent cells
      return static_cast<unsigned char>(refs_count_ + 8 * is_special_ +
                                        32 * level_mask.get_mask());
    }
    unsigned char d2() const {
      auto res = static_cast<unsigned char>((bits_ / 8) * 2);
      if ((bits_ & 7) != 0) {
        return static_cast<unsigned char>(res + 1);
      }
      return res;
    }
    size_t get_hashes_offset() const { return 0; }
    size_t get_refs_offset() const {
      return get_hashes_offset() + hash_bytes * hash_count_;
    }
    size_t get_depth_offset() const {
      return get_refs_offset() + refs_count_ * sizeof(Cell *);
    }
    size_t get_data_offset() const {
      return get_depth_offset() + sizeof(td::uint16) * hash_count_;
    }
    size_t get_storage_size() const {
      return get_data_offset() + (bits_ + 7) / 8;
    }

    const Hash *get_hashes(const char *storage) const {
      return reinterpret_cast<const Hash *>(storage + get_hashes_offset());
    }

    Hash *get_hashes(char *storage) const {
      return reinterpret_cast<Hash *>(storage + get_hashes_offset());
    }

    const td::uint16 *get_depth(const char *storage) const {
      return reinterpret_cast<const td::uint16 *>(storage + get_depth_offset());
    }

    td::uint16 *get_depth(char *storage) const {
      return reinterpret_cast<td::uint16 *>(storage + get_depth_offset());
    }

    const unsigned char *get_data(const char *storage) const {
      return reinterpret_cast<const unsigned char *>(storage +
                                                     get_data_offset());
    }
    unsigned char *get_data(char *storage) const {
      return reinterpret_cast<unsigned char *>(storage + get_data_offset());
    }

    Cell *const *get_refs(const char *storage) const {
      return reinterpret_cast<Cell *const *>(storage + get_refs_offset());
    }
    Cell **get_refs(char *storage) const {
      return reinterpret_cast<Cell **>(storage + get_refs_offset());
    }
  };

  Info info_;
  virtual char *get_storage() = 0;
  virtual const char *get_storage() const = 0;
  // TODO: we may also save three different pointers

  void destroy_storage(char *storage);

  explicit MyDataCell(Info info);

public:
  unsigned get_refs_cnt() const { return info_.refs_count_; }
  unsigned get_bits() const { return info_.bits_; }
  unsigned size_refs() const { return info_.refs_count_; }
  unsigned size() const { return info_.bits_; }
  const unsigned char *get_data() const {
    return info_.get_data(get_storage());
  }

  Cell *get_ref_raw_ptr(unsigned idx) const {
    DCHECK(idx < get_refs_cnt());
    return info_.get_refs(get_storage())[idx];
  }

  td::uint32 get_virtualization() const override {
    return info_.virtualization_;
  }
  bool is_loaded() const override { return true; }
  LevelMask get_level_mask() const override {
    return LevelMask{info_.level_mask_};
  }

  bool is_special() const { return info_.is_special_; }
  SpecialType special_type() const;
  int get_serialized_size(bool with_hashes = false) const {
    return ((get_bits() + 23) >> 3) +
           (with_hashes ? get_level_mask().get_hashes_count() *
                              (hash_bytes + depth_bytes)
                        : 0);
  }
  size_t get_storage_size() const { return info_.get_storage_size(); }

  std::pair<unsigned char, unsigned char>
  get_first_two_meta(bool with_hashes = false) const {
    return {static_cast<unsigned char>(info_.d1() | (with_hashes * 16)),
            info_.d2()};
  }

  int serialize(unsigned char *buff, int buff_size, bool with_hashes = false,
                bool with_meta = false) const {
    int len = get_serialized_size(with_hashes);
    if (len > buff_size) {
      return 0;
    }

    if (with_meta) {
      buff[0] = static_cast<unsigned char>(info_.d1() | (with_hashes * 16));
      buff[1] = info_.d2();
    }

    int hs = 0;
    if (with_hashes) {
      hs = (get_level_mask().get_hashes_count()) * (hash_bytes + depth_bytes);
      assert(len >= 2 + hs);
      std::memset(buff + 2, 0, hs);
      auto dest = td::MutableSlice(buff + 2, hs);
      auto level = get_level();
      // TODO: optimize for prunned brandh
      for (unsigned i = 0; i <= level; i++) {
        if (!get_level_mask().is_significant(i)) {
          continue;
        }
        dest.copy_from(get_hash(i).as_slice());
        dest.remove_prefix(hash_bytes);
      }
      for (unsigned i = 0; i <= level; i++) {
        if (!get_level_mask().is_significant(i)) {
          continue;
        }
        store_depth(dest.ubegin(), get_depth(i));
        dest.remove_prefix(depth_bytes);
      }
      // buff[2] = 0;  // for testing hash verification in deserialization
      buff += hs;
      len -= hs;
    }
    std::memcpy(buff + (with_meta ? 2 : 0), get_data(), len - 2);
    return len - (with_meta ? 0 : 2) + hs;
  }

  std::string serialize() const;
  std::string to_hex() const;

  template <class StorerT> void store(StorerT &storer) const {
    storer.template store_binary<td::uint8>(info_.d1());
    storer.template store_binary<td::uint8>(info_.d2());
    storer.store_slice(td::Slice(get_data(), (get_bits() + 7) / 8));
  }

protected:
  static constexpr auto max_storage_size =
      max_refs * sizeof(void *) + (max_level + 1) * hash_bytes + max_bytes;

private:
};

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
    data_offset = -1; // FIXME depth_offset + (with_hashes ? n *
                      // vm::Cell::depth_bytes : 0);
    data_len = (d2 >> 1) + (d2 & 1);
    data_with_bits = (d2 & 1) != 0;
    // refs_offset = data_offset + data_len;
    refs_offset = depth_offset + (with_hashes ? n * vm::Cell::depth_bytes : 0);
    end_offset = refs_offset + refs_cnt * ref_byte_size;

    return td::Status::OK();
  }
  td::Result<int> get_bits(td::Slice data) const {
    if (data_with_bits) {
      DCHECK(data_len != 0);
      int last = data[data_len - 1];
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
  create_data_cell(td::Slice cell_slice, td::Slice data_slice,
                   unsigned long long cell_data_offset,
                   td::Span<td::Ref<vm::Cell>> refs) const {
    vm::CellBuilder cb;
    TRY_RESULT(bits, get_bits(data_slice.substr(cell_data_offset)));
    cb.store_bits(data_slice.ubegin() + cell_data_offset, bits);

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
      assert(false);
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
      assert(false);
      return info.read_offset(index_ptr + (long)index * info.offset_byte_size);
    } else {
      assert(false);
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

  td::Result<int> get_cell_meta_size(int idx, td::Slice cells_slice) {}

  td::Result<td::Ref<vm::DataCell>>
  deserialize_cell(std::vector<int> idx_map, int idx, td::Slice cells_slice,
                   td::Slice data_slice, unsigned long long cell_data_offset,
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

    return cell_info.create_data_cell(cell_slice, data_slice, cell_data_offset,
                                      refs);
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
      assert(false);
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
        // std::cerr << "cur is " << cur << std::endl;
      }
      // TODO: it is OK to not reach the end
      // if (!cells_slice.empty()) {
      //   return td::Status::Error(PSLICE()
      //                            << "invalid bag-of-cells last cell #"
      //                            << info.cell_count - 1 << ": end offset "
      //                            << cur << " is different from total data
      //                            size "
      //                            << info.data_size);
      // }
    }
    unsigned long long meta_start = info.data_offset;
    unsigned long long meta_end = info.data_offset + custom_index.back();
    unsigned long long cell_data_start = meta_end;
    unsigned long long cell_data_end = info.data_offset + info.data_size;

    auto cells_slice = data.substr(meta_start, meta_end - meta_start);
    auto data_slice =
        data.substr(cell_data_start, cell_data_end - cell_data_start);

    std::vector<td::Ref<vm::DataCell>> cell_list;
    cell_list.reserve(cell_count);
    std::array<td::Ref<vm::Cell>, 4> refs_buf;

    auto topol_order = get_topol_order(cells_slice, cell_count).move_as_ok();
    std::vector<int> idx_map(cell_count, -1);
    std::vector<std::pair<unsigned long long, unsigned long long>>
        cell_data_interval;
    unsigned long long current_cell_data_offset = 0;

    for (int i = 0; i < info.cell_count; i++) {
      CellSerializationInfo cell_info;
      cell_info.init(cells_slice.substr((i ? custom_index[i - 1] : 0)),
                     info.ref_byte_size);
      unsigned long long cell_data_start = current_cell_data_offset;
      unsigned long long cell_data_end = cell_data_start + cell_info.data_len;
      cell_data_interval.emplace_back(cell_data_start, cell_data_end);
      current_cell_data_offset = cell_data_end;
    }

    for (auto &&idx : topol_order) {
      auto r_cell = deserialize_cell(
          idx_map, idx, cells_slice, data_slice, cell_data_interval[idx].first,
          cell_list, info.has_cache_bits ? &cell_should_cache : nullptr);
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

  td::uint64 compute_sizes(int mode, int &r_size, int &o_size) {
    int rs = 0, os = 0;
    if (!root_count || !data_bytes) {
      r_size = o_size = 0;
      return 0;
    }
    while (cell_count >= (1LL << (rs << 3))) {
      rs++;
    }
    td::uint64 hashes = (((mode & Mode::WithTopHash) ? top_hashes : 0) +
                         ((mode & Mode::WithIntHashes) ? int_hashes : 0)) *
                        (vm::Cell::hash_bytes + vm::Cell::depth_bytes);
    td::uint64 data_bytes_adj =
        data_bytes + (unsigned long long)int_refs * rs + hashes;
    td::uint64 max_offset =
        (mode & Mode::WithCacheBits) ? data_bytes_adj * 2 : data_bytes_adj;
    while (max_offset >= (1ULL << (os << 3))) {
      os++;
    }
    if (rs > 4 || os > 8) {
      r_size = o_size = 0;
      return 0;
    }
    r_size = rs;
    o_size = os;
    return data_bytes_adj;
  }

  std::size_t estimate_serialized_size(int mode) {
    if ((mode & Mode::WithCacheBits) && !(mode & Mode::WithIndex)) {
      info.invalidate();
      return 0;
    }
    auto data_bytes_adj =
        compute_sizes(mode, info.ref_byte_size, info.offset_byte_size);
    if (!data_bytes_adj) {
      info.invalidate();
      return 0;
    }
    info.valid = true;
    info.has_crc32c = mode & Mode::WithCRC32C;
    info.has_index = mode & Mode::WithIndex;
    info.has_cache_bits = mode & Mode::WithCacheBits;
    info.root_count = root_count;
    info.cell_count = cell_count;
    info.absent_count = dangle_count;
    int crc_size = info.has_crc32c ? 4 : 0;
    info.roots_offset =
        4 + 1 + 1 + 3 * info.ref_byte_size + info.offset_byte_size;
    info.index_offset =
        info.roots_offset + info.root_count * info.ref_byte_size;
    info.data_offset = info.index_offset;
    if (info.has_index) {
      info.data_offset += (long long)cell_count * info.offset_byte_size;
    }
    info.magic = Info::boc_generic;
    info.data_size = data_bytes_adj;
    info.total_size = info.data_offset + data_bytes_adj + crc_size;
    auto res = td::narrow_cast_safe<size_t>(info.total_size);
    if (res.is_error()) {
      return 0;
    }
    return res.ok();
  }

  td::Result<td::BufferSlice> serialize_to_slice(int mode) {
    std::size_t size_est = estimate_serialized_size(mode);
    if (!size_est) {
      return td::Status::Error("no cells to serialize to this bag of cells");
    }
    td::BufferSlice res(size_est);
    auto buff = const_cast<unsigned char *>(
        reinterpret_cast<const unsigned char *>(res.data()));

    TRY_RESULT(size, serialize_to(buff, res.size(), mode));

    if (size == res.size()) {
      return std::move(res);
    } else {
      return td::Status::Error("error while serializing a bag of cells: actual "
                               "serialized size differs from estimated");
    }
  }

  td::Result<std::size_t> serialize_to(unsigned char *buffer,
                                       std::size_t buff_size, int mode) {
    std::size_t size_est = estimate_serialized_size(mode);
    if (!size_est || size_est > buff_size) {
      return 0;
    }

    vm::boc_writers::BufferWriter writer{buffer, buffer + size_est};
    serialize_to_impl_only_meta(writer, mode);
    return serialize_to_impl_only_data(writer, mode);
  }

  template <typename WriterT>
  td::Result<std::size_t> serialize_to_impl_only_data(WriterT &writer,
                                                      int mode) {
    for (int i = 0; i < cell_count; ++i) {
      const auto &dc_info = cell_list_[cell_count - 1 - i];
      const td::Ref<vm::DataCell> &dc = dc_info.dc_ref;
      bool with_hash = (mode & Mode::WithIntHashes) && !dc_info.wt;
      if (dc_info.is_root_cell && (mode & Mode::WithTopHash)) {
        with_hash = true;
      }
      unsigned char buf[256];
      auto myDc = reinterpret_cast<const MyDataCell *>(dc.get());
      int s = myDc->serialize(buf, 256, with_hash, false);
      writer.store_bytes(buf, s);
    }

    return writer.position();
  }

  // serialized_boc#672fb0ac has_idx:(## 1) has_crc32c:(## 1)
  //  has_cache_bits:(## 1) flags:(## 2) { flags = 0 }
  //  size:(## 3) { size <= 4 }
  //  off_bytes:(## 8) { off_bytes <= 8 }
  //  cells:(##(size * 8))
  //  roots:(##(size * 8))
  //  absent:(##(size * 8)) { roots + absent <= cells }
  //  tot_cells_size:(##(off_bytes * 8))
  //  index:(cells * ##(off_bytes * 8))
  //  cell_data:(tot_cells_size * [ uint8 ])
  //  = BagOfCells;
  // Changes in this function may require corresponding changes in
  // crypto/vm/large-boc-serializer.cpp
  template <typename WriterT>
  td::Result<std::size_t> serialize_to_impl_only_meta(WriterT &writer,
                                                      int mode) {
    auto store_ref = [&](unsigned long long value) {
      writer.store_uint(value, info.ref_byte_size);
    };
    auto store_offset = [&](unsigned long long value) {
      writer.store_uint(value, info.offset_byte_size);
    };

    writer.store_uint(info.magic, 4);

    td::uint8 byte{0};
    if (info.has_index) {
      byte |= 1 << 7;
    }
    if (info.has_crc32c) {
      byte |= 1 << 6;
    }
    if (info.has_cache_bits) {
      byte |= 1 << 5;
    }
    // 3, 4 - flags
    if (info.ref_byte_size < 1 || info.ref_byte_size > 7) {
      return 0;
    }
    byte |= static_cast<td::uint8>(info.ref_byte_size);
    writer.store_uint(byte, 1);

    writer.store_uint(info.offset_byte_size, 1);
    store_ref(cell_count);
    store_ref(root_count);
    store_ref(0);
    store_offset(info.data_size);
    for (const auto &root_info : roots) {
      int k = cell_count - 1 - root_info.idx;
      DCHECK(k >= 0 && k < cell_count);
      store_ref(k);
    }
    DCHECK(writer.position() == info.index_offset);
    DCHECK((unsigned)cell_count == cell_list_.size());
    if (info.has_index) {
      std::size_t offs = 0;
      for (int i = cell_count - 1; i >= 0; --i) {
        const td::Ref<vm::DataCell> &dc = cell_list_[i].dc_ref;
        bool with_hash = (mode & Mode::WithIntHashes) && !cell_list_[i].wt;
        if (cell_list_[i].is_root_cell && (mode & Mode::WithTopHash)) {
          with_hash = true;
        }
        offs += dc->get_serialized_size(with_hash) +
                dc->size_refs() * info.ref_byte_size;
        auto fixed_offset = offs;
        if (info.has_cache_bits) {
          fixed_offset = offs * 2 + cell_list_[i].should_cache;
        }
        store_offset(fixed_offset);
      }
      DCHECK(offs == info.data_size);
    }
    DCHECK(writer.position() == info.data_offset);
    size_t keep_position = writer.position();
    for (int i = 0; i < cell_count; ++i) {
      const auto &dc_info = cell_list_[cell_count - 1 - i];
      const td::Ref<vm::DataCell> &dc = dc_info.dc_ref;
      bool with_hash = (mode & Mode::WithIntHashes) && !dc_info.wt;
      if (dc_info.is_root_cell && (mode & Mode::WithTopHash)) {
        with_hash = true;
      }
      // unsigned char buf[256];
      // int s = dc->serialize(buf, 256, with_hash);
      // writer.store_bytes(buf, s);
      auto myDc = reinterpret_cast<const MyDataCell *>(dc.get());
      unsigned char buf[2];
      auto first_two_meta = myDc->get_first_two_meta(with_hash);
      buf[0] = first_two_meta.first;
      buf[1] = first_two_meta.second;
      writer.store_bytes(buf, 2);

      DCHECK(dc->size_refs() == dc_info.ref_num);
      // std::cerr << (dc_info.is_special() ? '*' : ' ') << i << '<' <<
      // (int)dc_info.wt << ">:";
      for (unsigned j = 0; j < dc_info.ref_num; ++j) {
        int k = cell_count - 1 - dc_info.ref_idx[j];
        DCHECK(k > i && k < cell_count);
        store_ref(k);
        // std::cerr << ' ' << k;
      }
      // std::cerr << std::endl;
    }
    writer.chk();
    DCHECK(writer.position() - keep_position == info.data_size);
    DCHECK(writer.remaining() == (info.has_crc32c ? 4 : 0));
    if (info.has_crc32c) {
      unsigned crc = writer.get_crc32();
      writer.store_uint(td::bswap32(crc), 4);
    }
    DCHECK(writer.empty());
    return writer.position();
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

td::Result<td::BufferSlice> my_std_boc_serialize(td::Ref<vm::Cell> root,
                                                 int mode = 0) {
  if (root.is_null()) {
    return td::Status::Error(
        "cannot serialize a null cell reference into a bag of cells");
  }
  vm::BagOfCells boc;
  boc.add_root(std::move(root));
  auto res = boc.import_cells();

  auto myBoc = reinterpret_cast<MyBagOfCells *>(&boc);

  if (res.is_error()) {
    return res.move_as_error();
  }
  return myBoc->serialize_to_slice(mode);
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

td::BufferSlice compress(td::Slice data) {
  td::Ref<vm::Cell> root = vm::std_boc_deserialize(data).move_as_ok();
  return td::lz4_compress(my_std_boc_serialize(root, 0).move_as_ok());
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
