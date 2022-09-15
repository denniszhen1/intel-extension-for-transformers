//  Copyright (c) 2021 Intel Corporation
//
//  Licensed under the Apache License, Version 2.0 (the "License");
//  you may not use this file except in compliance with the License.
//  You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
//  Unless required by applicable law or agreed to in writing, software
//  distributed under the License is distributed on an "AS IS" BASIS,
//  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//  See the License for the specific language governing permissions and
//  limitations under the License.

#ifndef ENGINE_EXECUTOR_INCLUDE_TENSOR_HPP_
#define ENGINE_EXECUTOR_INCLUDE_TENSOR_HPP_
#include <algorithm>
#include <functional>
#include <numeric>
#include <string>
#include <vector>
#include <unordered_map>

#include "common.hpp"
#include "conf.hpp"
#include "memory_allocator.hpp"

namespace executor {
/**
 * @brief A wrapper around Memory holders serving as the basic
 *        computational unit through which Operator%s, Model%s interact.
 *
 */
class Tensor {
 public:
  Tensor(void* data, const vector<int64_t>& shape, const string& dtype, const vector<int64_t>& strides = {},
         const vector<int64_t>& location = {}, const string& name = "")
      : name_(name), data_(data), shape_(shape), dtype_(dtype), location_(location), strides_(strides) {}

  // for pybind caster
  Tensor() : data_(nullptr), shape_({}), dtype_("fp32"), name_("") {}

  explicit Tensor(const TensorConfig& tensor_config) : data_(nullptr) {
    name_ = tensor_config.name();
    shape_ = tensor_config.shape();
    location_ = tensor_config.location();
    dtype_ = tensor_config.dtype();
    strides_ = tensor_config.strides();
  }
  // use data after set_shape
  inline const void* data() {
    if (shm_handle_ != 0) {
      data_ = MemoryAllocator::ManagedShm().get_address_from_handle(shm_handle_);
    }
    if (data_ == nullptr) {
      data_ = MemoryAllocator::get().GetMemory(this->size() * type2bytes[this->dtype()], this->life());
      // MemoryAllocator::get().SetName(data_, this->name());
    }
    return data_;
  }
  inline void* mutable_data() {
    if (shm_handle_ != 0) {
      data_ = MemoryAllocator::ManagedShm().get_address_from_handle(shm_handle_);
    }
    if (data_ == nullptr) {
      data_ = MemoryAllocator::get().GetMemory(this->size() * type2bytes[this->dtype()], this->life());
      // MemoryAllocator::get().SetName(data_, this->name());
    }
    return data_;
  }

  void set_data(void* data) {
    if (data_ != nullptr) MemoryAllocator::get().ResetMemory(data_, 0);
    auto exists_status = MemoryAllocator::get().CheckMemory(data);
    if (exists_status != -1) {
      if (disposable_life_count_ != 0) {
        MemoryAllocator::get().ResetMemory(data, disposable_life_count_);
        disposable_life_count_ = 0;
      } else {
        MemoryAllocator::get().ResetMemory(data, this->life());
      }
    }
    data_ = data;
  }

  int unref_data(bool inplace = false) {
    // weight tensor no need to unref
    if (!location_.empty()) return 0;
    auto status = MemoryAllocator::get().UnrefMemory(data_, inplace);
    // if we got status == -1, will keep the pointer
    if (status == 0) data_ = nullptr;
    return status;
  }

  void set_name(const string& name) { name_ = name; }

  void set_shape(const vector<int64_t>& shape) {
    shape_ = shape;
    refresh_hash_ = true;
  }

  void set_dtype(const string& dtype) {
    dtype_ = dtype;
    refresh_hash_ = true;
  }

  // get tensor hash key for dispatcher
  // combine shape and dtype
  size_t get_hash() {
    if (refresh_hash_) {
      vector<int64_t> hash_vec(shape_);
      hash_vec.push_back(type2bytes[dtype_]);
      size_t seed = hash_vec.size();
      hash_ = get_array_hash(seed, hash_vec, hash_vec.size());
      refresh_hash_ = false;
    }
    return hash_;
  }

  void set_transpose(const bool& transpose = true) { is_transposed_ = transpose; }

  void add_tensor_life(const int count) { life_count_ += count; }

  // add extra tensor life for op tuning
  // need to reset to real life_count after tuning
  void disposable_extra_life(const int count) {
    disposable_life_count_ = life_count_ + count;
    if (location_.empty() && data_ != nullptr) {
      auto exists_status = MemoryAllocator::get().CheckMemory(data_);
      if (exists_status != -1) {
        MemoryAllocator::get().ResetMemory(data_, exists_status + count);
        disposable_life_count_ = 0;
        return;
      }
    }
    LOG(WARNING) << "please set tensor data to make adding extra tensor life work...";
  }

  // reorder tensor
  // for example, use reorder in sparselib, 2D for transpose mode, 3D for tuning and dispatch
  // [a, b] -> [b, a], dst_perm: [1, 0]
  // [a, b, c] -> [b, a, c], dst_perm: [1, 0 ,2]
  void reorder(const vector<int64_t>& src_shape, const vector<int64_t>& dst_perm = {1, 0, 2}) {
    static unordered_map<string, dnnl::memory::data_type> type2mem{
    {"fp32", dnnl::memory::data_type::f32}, {"s32", dnnl::memory::data_type::s32},
    {"fp16", dnnl::memory::data_type::f16}, {"u8", dnnl::memory::data_type::u8},
    {"s8", dnnl::memory::data_type::s8},    {"bf16", dnnl::memory::data_type::bf16}};
    // execute reorder primitive
    vector<int64_t> src_stride = GetStrides(src_shape);
    vector<int64_t> dst_shape = GetShapes(src_shape, dst_perm);
    vector<int64_t> dst_stride = GetStrides(dst_shape, ReversePerm(dst_perm));
    dnnl::memory::desc src_md(src_shape, type2mem[this->dtype()], src_stride);
    dnnl::memory::desc dst_md(src_shape, type2mem[this->dtype()], dst_stride);
    dnnl::engine reorder_eng(dnnl::engine::kind::cpu, 0);
    dnnl::stream reorder_eng_stream(reorder_eng);
    dnnl::reorder::primitive_desc reorder_src_pd(reorder_eng, src_md, reorder_eng, dst_md);
    dnnl::memory src_m(src_md, reorder_eng);
    dnnl::memory dst_m(dst_md, reorder_eng);
    src_m.set_data_handle(const_cast<void*>(this->data()), reorder_eng_stream);
    dnnl::reorder(src_m, dst_m).execute(reorder_eng_stream, src_m, dst_m);
    reorder_eng_stream.wait();
    // inplace dst
    void* p = dst_m.get_data_handle();
    size_t data_size = this->size() * type2bytes[this->dtype()];
    memcpy(this->mutable_data(), p, data_size);
  }

  inline size_t size() { return std::accumulate(shape_.begin(), shape_.end(), size_t(1), std::multiplies<size_t>()); }

  void set_shm_handle(const ipc::managed_shared_memory::handle_t& h) { shm_handle_ = h; }
  bool is_shared() { return shm_handle_ != 0; }

  inline const string& name() const { return name_; }
  inline const int life() const { return life_count_; }
  inline const int left_life() const {
    return MemoryAllocator::get().CheckMemory(data_);
  }  // return -1 represent the data should always be hold.
  inline const void* raw_data() const { return data_; }
  inline const vector<int64_t>& shape() const { return shape_; }
  inline const vector<int64_t>& location() const { return location_; }
  inline const string& dtype() const { return dtype_; }
  inline const vector<int64_t>& strides() const { return strides_; }
  inline const bool& is_transposed() const { return is_transposed_; }

 protected:
  string name_;
  void* data_;
  vector<int64_t> shape_;
  string dtype_;
  // for dispatcher, combine shape and dtype
  bool refresh_hash_ = true;
  size_t hash_ = 0;
  vector<int64_t> location_;
  vector<int64_t> strides_;
  bool is_transposed_ = false;

  // for memory handling
  int life_count_ = 0;
  // for op tuning memory handling
  int disposable_life_count_ = 0;

  // If shm_handle_ not equal to 0, which means it is on shared memory
  ipc::managed_shared_memory::handle_t shm_handle_ = 0;
};  // class Tensor
}  // namespace executor

#endif  // ENGINE_EXECUTOR_INCLUDE_TENSOR_HPP_
