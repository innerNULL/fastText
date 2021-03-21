/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "productquantizer.h"

#include <algorithm>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <string>

namespace fasttext {

real distL2(const real* x, const real* y, int32_t d) {
  real dist = 0;
  for (auto i = 0; i < d; i++) {
    auto tmp = x[i] - y[i];
    dist += tmp * tmp;
  }
  return dist;
}

ProductQuantizer::ProductQuantizer(int32_t dim, int32_t dsub)
    : dim_(dim),
      nsubq_(dim / dsub),
      dsub_(dsub),
      centroids_(dim * ksub_),
      rng(seed_) {
  lastdsub_ = dim_ % dsub;
  if (lastdsub_ == 0) {
    lastdsub_ = dsub_;
  } else {
    nsubq_++;
  }
}

const real* ProductQuantizer::get_centroids(int32_t m, uint8_t i) const {
  if (m == nsubq_ - 1) {
    return &centroids_[m * ksub_ * dsub_ + i * lastdsub_];
  }
  return &centroids_[(m * ksub_ + i) * dsub_];
}

real* ProductQuantizer::get_centroids(int32_t m, uint8_t i) {
  if (m == nsubq_ - 1) {
    return &centroids_[m * ksub_ * dsub_ + i * lastdsub_];
  }
  return &centroids_[(m * ksub_ + i) * dsub_];
}

real ProductQuantizer::assign_centroid(
    const real* x,
    const real* c0,
    uint8_t* code,
    int32_t d) const {
  const real* c = c0;
  real dis = distL2(x, c, d);
  code[0] = 0;
  for (auto j = 1; j < ksub_; j++) {
    c += d;
    real disij = distL2(x, c, d);
    if (disij < dis) {
      code[0] = (uint8_t)j;
      dis = disij;
    }
  }
  return dis;
}

void ProductQuantizer::Estep(
    const real* x,
    const real* centroids,
    uint8_t* codes,
    int32_t d,
    int32_t n) const {
  for (auto i = 0; i < n; i++) {
    assign_centroid(x + i * d, centroids, codes + i, d);
  }
}

void ProductQuantizer::MStep(
    const real* x0,
    real* centroids,
    const uint8_t* codes,
    int32_t d,
    int32_t n) {
  std::vector<int32_t> nelts(ksub_, 0);
  memset(centroids, 0, sizeof(real) * d * ksub_);
  const real* x = x0;
  for (auto i = 0; i < n; i++) {
    auto k = codes[i];
    real* c = centroids + k * d;
    for (auto j = 0; j < d; j++) {
      c[j] += x[j];
    }
    nelts[k]++;
    x += d;
  }

  real* c = centroids;
  for (auto k = 0; k < ksub_; k++) {
    real z = (real)nelts[k];
    if (z != 0) {
      for (auto j = 0; j < d; j++) {
        c[j] /= z;
      }
    }
    c += d;
  }

  std::uniform_real_distribution<> runiform(0, 1);
  for (auto k = 0; k < ksub_; k++) {
    if (nelts[k] == 0) {
      int32_t m = 0;
      while (runiform(rng) * (n - ksub_) >= nelts[m] - 1) {
        m = (m + 1) % ksub_;
      }
      memcpy(centroids + k * d, centroids + m * d, sizeof(real) * d);
      for (auto j = 0; j < d; j++) {
        int32_t sign = (j % 2) * 2 - 1;
        centroids[k * d + j] += sign * eps_;
        centroids[m * d + j] -= sign * eps_;
      }
      nelts[k] = nelts[m] / 2;
      nelts[m] -= nelts[k];
    }
  }
}

void ProductQuantizer::kmeans(const real* x, real* c, int32_t n, int32_t d) {
  std::vector<int32_t> perm(n, 0);
  std::iota(perm.begin(), perm.end(), 0);
  std::shuffle(perm.begin(), perm.end(), rng);
  for (auto i = 0; i < ksub_; i++) {
    memcpy(&c[i * d], x + perm[i] * d, d * sizeof(real));
  }
  auto codes = std::vector<uint8_t>(n);
  for (auto i = 0; i < niter_; i++) {
    Estep(x, c, codes.data(), d, n);
    MStep(x, c, codes.data(), d, n);
  }
}

/**
 * @brief
 * Training process for a Prodcut-Quantization. The process mainly training 
 * a k-means model on an one-dimentsion vector, precisely, mainly executed 
 * on an l2-norm vector for embedding-matrix.
 *
 * @param x
 * The pointer points to the first element of a `Vector` or `Matrix` object.
 * 
 * When doing PQ for l2-norm vector of embedding-matrix, then PQ target is 
 * `n` numbers of elements in `Vector`, each element value represents l2-norm 
 * corresponding one embedding vector in embedding-matrix, and during the 
 * k-means process in PQ, each element will be handled as an one-dimension 
 * vector.
 *
 * When doing PQ for embedding-matrix, the PQ target is a `Matrix` object. 
 * In this case, the minimum data unit is each embedding-vector.
 *
 * @param n
 * The number of records using to execute prodcut-quantization process. 
 * `n` is usually same with the embedding vector number.
 */
void ProductQuantizer::train(int32_t n, const real* x) {
  /// The number of items waiting for splitting to several sub-vectors 
  /// (or sub-elements) and then processing k-means should be larger than 
  /// target k-means cluster number.
  if (n < ksub_) {
    throw std::invalid_argument(
        "Matrix too small for quantization, must have at least " +
        std::to_string(ksub_) + " rows");
  }
  std::vector<int32_t> perm(n, 0);
  /// TODO: `std::iota` will let `perm` be an incremental series of numbers, 
  /// such as [0, 1, 2, ..., n-1]. But why?
  std::iota(perm.begin(), perm.end(), 0);
  /// `dsub_` means "dimension of subquantizers/subvectors/subspaces", which 
  /// is the dimension of each sub-vector(sub-space) in product-quantization 
  /// process. 
  ///
  /// In the case of l2-norm vector prodcut-quantization, `dsub_` equals 1.
  /// In the case of embedding matrix prodcut-quantization, `dsub_` equals 
  /// embedding size (embedding-dimension). 
  auto d = dsub_;
  /// `np` means "number of points" for each iteration EM training of k-means, 
  /// similiar with the notion of "batch-size". But the difference is these 
  /// "batch" is not a subset splited from full data, but sampled from the full 
  /// data. 
  ///
  /// For example, suppose we has `n` embedding vectors, and we want training 
  /// a k-means model for each subquantizer by EM algorithm. If we set, for 
  /// each EM training iteration we the training data size is `np`, what 
  /// fastText do is sampling `np` samples from full data with size `n` by:
  ///   * Shuffling input original matrix (or vector in l2-norm PQ case)
  ///   * Using first `np` sample from shuffled data as training data for 
  ///     current EM training iteration.
  ///
  /// So, the max value of `np` can not larger than `n`, which is the number 
  /// of embedding vectors. The extreme case we only execute single iteration 
  /// EM training algorithm and feeding all data as a "huge" batch with size 
  /// as `n`.
  auto np = std::min(n, max_points_);
  /// TODO:
  /// `xslice` is the container for holding each training-iteration's training 
  /// data.
  /// A l2-norm vector product-quantization example, suggest we have n embedding 
  /// vectors' l2-norm, and each EM k-means training-iteration's number of data 
  /// `np`  
  auto xslice = std::vector<real>(np * dsub_);
  /// `nsubq_` means "number of subquantizers", which is same with 
  /// subvector number.
  /// The following for-loop block iterates along each subvector and training 
  /// corresponding subquantizer.
  ///
  /// One important fact is, when executing PQ on embedding-matrix's l2-norm 
  /// vector, the target of PQ is not conventional vector, but some scalars 
  /// which represent certain embedding-vectors l2-norm value. At this time, 
  /// `ProductQuantizer::train` will regards these scalars as one-dimension 
  /// vectors and executring k-means on them during PQ process. And at the 
  /// same time, since each "vector" is an one-dimension vector, so the number 
  /// of subquantizer/subvector/subspace can only be 1. 
  for (auto m = 0; m < nsubq_; m++) { 
    /// Mark the last subquantizer. 
    if (m == nsubq_ - 1) {
      d = lastdsub_;
    }
    /// TODO:
    /// Shuffle the k-means training "samples", with each block of `rng` number 
    /// of element as the minimum shuffle unit.
    /// In 
    if (np != n) {
      std::shuffle(perm.begin(), perm.end(), rng);
    }
    for (auto j = 0; j < np; j++) {
      memcpy(
          /// j-th sample's vector starting address.
          xslice.data() + j * d,
          /// We can decompose the follow line's expression into several parts:
          /// `x`: 
          ///     The start pointer of input data, which points the address of 
          ///     the first input element.
          /// `x + perm[j] * dim_`: 
          ///     The starting pointer of current target PQ vector.
          ///  
          x + perm[j] * dim_ + m * dsub_,
          d * sizeof(real));
    }
    kmeans(xslice.data(), get_centroids(m, 0), np, d);
  }
}

real ProductQuantizer::mulcode(
    const Vector& x,
    const uint8_t* codes,
    int32_t t,
    real alpha) const {
  real res = 0.0;
  auto d = dsub_;
  const uint8_t* code = codes + nsubq_ * t;
  for (auto m = 0; m < nsubq_; m++) {
    const real* c = get_centroids(m, code[m]);
    if (m == nsubq_ - 1) {
      d = lastdsub_;
    }
    for (auto n = 0; n < d; n++) {
      res += x[m * dsub_ + n] * c[n];
    }
  }
  return res * alpha;
}

void ProductQuantizer::addcode(
    Vector& x,
    const uint8_t* codes,
    int32_t t,
    real alpha) const {
  auto d = dsub_;
  const uint8_t* code = codes + nsubq_ * t;
  for (auto m = 0; m < nsubq_; m++) {
    const real* c = get_centroids(m, code[m]);
    if (m == nsubq_ - 1) {
      d = lastdsub_;
    }
    for (auto n = 0; n < d; n++) {
      x[m * dsub_ + n] += alpha * c[n];
    }
  }
}

void ProductQuantizer::compute_code(const real* x, uint8_t* code) const {
  auto d = dsub_;
  for (auto m = 0; m < nsubq_; m++) {
    if (m == nsubq_ - 1) {
      d = lastdsub_;
    }
    assign_centroid(x + m * dsub_, get_centroids(m, 0), code + m, d);
  }
}

void ProductQuantizer::compute_codes(const real* x, uint8_t* codes, int32_t n)
    const {
  for (auto i = 0; i < n; i++) {
    compute_code(x + i * dim_, codes + i * nsubq_);
  }
}

void ProductQuantizer::save(std::ostream& out) const {
  out.write((char*)&dim_, sizeof(dim_));
  out.write((char*)&nsubq_, sizeof(nsubq_));
  out.write((char*)&dsub_, sizeof(dsub_));
  out.write((char*)&lastdsub_, sizeof(lastdsub_));
  out.write((char*)centroids_.data(), centroids_.size() * sizeof(real));
}

void ProductQuantizer::load(std::istream& in) {
  in.read((char*)&dim_, sizeof(dim_));
  in.read((char*)&nsubq_, sizeof(nsubq_));
  in.read((char*)&dsub_, sizeof(dsub_));
  in.read((char*)&lastdsub_, sizeof(lastdsub_));
  centroids_.resize(dim_ * ksub_);
  for (auto i = 0; i < centroids_.size(); i++) {
    in.read((char*)&centroids_[i], sizeof(real));
  }
}

} // namespace fasttext
