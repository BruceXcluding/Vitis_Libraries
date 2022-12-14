// Copyright (c) the JPEG XL Project Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include "acc_enc_cluster.hpp"

#include <ap_int.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <queue>
#include <tuple>

#undef HWY_TARGET_INCLUDE
#define HWY_TARGET_INCLUDE "enc_cluster.cpp"
#include <hwy/foreach_target.h>
#include <hwy/highway.h>

#include "lib/jxl/ac_context.h"
#include "lib/jxl/base/profiler.h"
#include "lib/jxl/fast_math-inl.h"
HWY_BEFORE_NAMESPACE();
namespace jxl {
namespace HWY_NAMESPACE {

template <class V>
V Entropy(V count, V inv_total, V total) {
    const HWY_CAPPED(float, Histogram::kRounding) d;
    const auto zero = Set(d, 0.0f);
    return IfThenZeroElse(count == total,
                          zero - count * FastLog2f(d, count) +
                              count * FastLog2f(d, total)); // zero-count*FastLog2f(d, inv_total * count));
}

void HistogramEntropy(const Histogram& a) {
    a.entropy_ = 0.0f;
    if (a.total_count_ == 0) return;

    const HWY_CAPPED(float, Histogram::kRounding) df;
    const HWY_CAPPED(int32_t, Histogram::kRounding) di;

    const auto inv_tot = Set(df, 1.0f / a.total_count_);
    auto entropy_lanes = Zero(df);
    auto total = Set(df, a.total_count_);
    // printf("%s: %s: %d, a.data_.size=%d\n", __FILE__, __FUNCTION__, __LINE__,
    // a.data_.size());
    for (size_t i = 0; i < a.data_.size(); i += Lanes(di)) {
        const auto counts = LoadU(di, &a.data_[i]);
        entropy_lanes += Entropy(ConvertTo(df, counts), inv_tot, total);
    }
    a.entropy_ += GetLane(SumOfLanes(entropy_lanes));
}

float HistogramDistance(const Histogram& a, const Histogram& b) {
    if (a.total_count_ == 0 || b.total_count_ == 0) return 0;

    const HWY_CAPPED(float, Histogram::kRounding) df;
    const HWY_CAPPED(int32_t, Histogram::kRounding) di;

    const auto inv_tot = Set(df, 1.0f / (a.total_count_ + b.total_count_));
    auto distance_lanes = Zero(df);
    auto total = Set(df, a.total_count_ + b.total_count_);

    for (size_t i = 0; i < std::max(a.data_.size(), b.data_.size()); i += Lanes(di)) {
        const auto a_counts = a.data_.size() > i ? LoadU(di, &a.data_[i]) : Zero(di);
        const auto b_counts = b.data_.size() > i ? LoadU(di, &b.data_[i]) : Zero(di);
        const auto counts = ConvertTo(df, a_counts + b_counts);
        distance_lanes += Entropy(counts, inv_tot, total);
    }
    const float total_distance = GetLane(SumOfLanes(distance_lanes));
    return total_distance - a.entropy_ - b.entropy_;
}

// First step of a k-means clustering with a fancy distance metric.
/*void FastClusterHistograms(const std::vector<Histogram>& in,
                           const size_t num_contexts_in, size_t max_histograms,
                           float min_distance, std::vector<Histogram>* out,
                           std::vector<uint32_t>* histogram_symbols) {
  PROFILER_FUNC;
  size_t largest_idx = 0;
  std::vector<uint32_t> nonempty_histograms;
  nonempty_histograms.reserve(in.size());
  int largest_count = 0;
  printf("%s: %s: %d, num_contexts_in=%d\n", __FILE__, __FUNCTION__, __LINE__,
num_contexts_in); for (size_t i = 0; i < num_contexts_in; i++) {  // get
position for largest total_count_ id in in if (in[i].total_count_ == 0)
continue; HistogramEntropy(in[i]); if (in[i].total_count_ >
in[largest_idx].total_count_) { largest_idx = i; largest_count =
in[i].total_count_;
    }
    nonempty_histograms.push_back(i);
  }
  // No symbols.
  if (nonempty_histograms.empty()) {
    out->resize(1);
    histogram_symbols->clear();
    histogram_symbols->resize(in.size(), 0);
    return;
  }
  largest_idx = std::find(nonempty_histograms.begin(),
                          nonempty_histograms.end(), largest_idx) -
                nonempty_histograms.begin(); // get position for largest
total_count_ id in nonempty_histograms size_t num_contexts =
nonempty_histograms.size(); printf("%s: %s: %d, num_contexts of non-empty=%d,
largest_idx=%d, largest_count=%d\n", __FILE__, __FUNCTION__, __LINE__,
    num_contexts, largest_idx, largest_count);
  out->clear();
  out->reserve(max_histograms);
  std::vector<float> dists(num_contexts, std::numeric_limits<float>::max());
  histogram_symbols->resize(in.size(), max_histograms);

  int while_count = 0;
  while (out->size() < max_histograms && out->size() < num_contexts) {
    (*histogram_symbols)[nonempty_histograms[largest_idx]] = out->size();
    out->push_back(in[nonempty_histograms[largest_idx]]);
    largest_idx = 0;
    while_count++;
    for (size_t i = 0; i < num_contexts; i++) {
      dists[i] = std::min(
          HistogramDistance(in[nonempty_histograms[i]], out->back()), dists[i]);
      // Avoid repeating histograms
      if ((*histogram_symbols)[nonempty_histograms[i]] != max_histograms) {
        continue;
      }
      if (dists[i] > dists[largest_idx]) largest_idx = i;
    }
    if (dists[largest_idx] < min_distance) break;
  }

  for (size_t i = 0; i < num_contexts_in; i++) {
    if ((*histogram_symbols)[i] != max_histograms) continue;
    if (in[i].total_count_ == 0) {
      (*histogram_symbols)[i] = 0;
      continue;
    }
    size_t best = 0;
    float best_dist = HistogramDistance(in[i], (*out)[best]);
    for (size_t j = 1; j < out->size(); j++) {
      float dist = HistogramDistance(in[i], (*out)[j]);
      if (dist < best_dist) {
        best = j;
        best_dist = dist;
      }
    }
    (*out)[best].AddHistogram(in[i]);
    HistogramEntropy((*out)[best]);
    (*histogram_symbols)[i] = best;
  }

  printf("%s: %s: %d, out size=%zu, FastClusterHistograms size=%zu,
while_count=%d\n", __FILE__, __FUNCTION__, __LINE__, out->size(),
histogram_symbols->size(), while_count);
}*/

float accHistogramDistanceEntropy(const Histogram& a, const Histogram& b, bool isEntropy) {
    if (!isEntropy) {
        if (a.total_count_ == 0 || b.total_count_ == 0) return 0;
    } else {
        a.entropy_ = 0.0f;
        if (a.total_count_ == 0) return 0;
    }

    float total;
    if (!isEntropy) {
        total = a.total_count_ + b.total_count_;
    } else {
        total = a.total_count_;
    }
    float totallog2 = total == 0 ? 0 : std::log2(total) /*acc::log2(total)*/;
    float distance_lanes = 0;
    size_t sum_count = 0;
    float sum_dist = 0;

    size_t size;
    if (!isEntropy) {
        size = std::max(a.data_.size(), b.data_.size());
    } else {
        size = a.data_.size();
    }

    for (size_t i = 0; i < size; i++) {
        float counts;
        if (!isEntropy) {
            size_t a_counts = a.data_.size() > i ? a.data_[i] : 0;
            size_t b_counts = b.data_.size() > i ? b.data_[i] : 0;
            counts = a_counts + b_counts;
        } else {
            counts = a.data_[i];
        }

        float countlog2 = counts == 0 ? 0 : /*acc::log2(counts)*/ std::log2(counts);

        sum_count += counts == total ? 0 : counts;
        sum_dist += counts == total ? 0 : counts * countlog2;
    }
    distance_lanes = sum_count * totallog2 - sum_dist;
    float result;
    if (!isEntropy) {
        result = distance_lanes - a.entropy_ - b.entropy_;
    } else {
        result = distance_lanes;
    }
    return result;
}

// clang-format off
float accHistogramDistanceEntropy(
#ifndef __SYNTHESIS__
                                  bool isEntropy, 
                                  int32_t a_size,
                                  int32_t a_total_count,
                                  std::vector<int32_t> a_histo, 
                                  int32_t b_size,
                                  int32_t b_total_count,
                                  std::vector<int32_t> b_histo
#else
                                  bool isEntropy, 
                                  int32_t a_size,
                                  int32_t a_total_count,
                                  a_histo[40], 
                                  int32_t b_size,
                                  int32_t b_total_count,
                                  b_histo[40]
#endif
) {
    // clang-format on
    if (!isEntropy) {
        if (a_total_count == 0 || b_total_count == 0) return 0;
    } else {
        if (a_total_count == 0) return 0;
    }

    float total;
    if (!isEntropy) {
        total = a_total_count + b_total_count;
    } else {
        total = a_total_count;
    }
    float totallog2 = total == 0 ? 0 : /*acc::log2(total)*/ std::log2(total);
    float distance_lanes = 0;
    size_t sum_count = 0;
    float sum_dist = 0;

    size_t size;
    if (!isEntropy) {
        size = std::max(a_size, b_size);
    } else {
        size = a_size;
    }

    for (size_t i = 0; i < size; i++) {
        float counts;
        if (!isEntropy) {
            size_t a_counts = a_size > i ? a_histo[i] : 0;
            size_t b_counts = b_size > i ? b_histo[i] : 0;
            counts = a_counts + b_counts;
        } else {
            counts = a_histo[i];
        }

        float countlog2 = counts == 0 ? 0 : /*acc::log2(counts)*/ std::log2(counts);

        sum_count += counts == total ? 0 : counts;
        sum_dist += counts == total ? 0 : counts * countlog2;
    }
    distance_lanes = sum_count * totallog2 - sum_dist;
    return distance_lanes;
}

void acc_HistogramDistance(bool isEntropy,
                           size_t num_contexts,
                           size_t j,
                           const std::vector<Histogram> in,
                           std::vector<uint32_t> nonempty_histograms,
                           Histogram& ref,
                           std::vector<float>& dists,
                           std::vector<size_t>& best,
                           size_t& largest_idx) {
    largest_idx = 0;
    for (size_t i = 0; i < num_contexts; i++) {
        const Histogram a = in[nonempty_histograms[i]];
        float dist_std = accHistogramDistanceEntropy(isEntropy, a.data_.size(), a.total_count_, a.data_,
                                                     ref.data_.size(), ref.total_count_, ref.data_);
        if (!isEntropy) {
            if (dist_std - a.entropy_ - ref.entropy_ < dists[i]) {
                best[i] = j;
                dists[i] = dist_std - a.entropy_ - ref.entropy_;
            }
        } else {
            dists[i] = dist_std;
        }
        if (dists[i] > dists[largest_idx]) largest_idx = i;
    }
}

// clang-format off
void acc_HistogramDistance(
#ifndef __SYNTHESIS__
                           bool isEntropy, uint32_t num_contexts, uint32_t j,

                           const std::vector<uint32_t> acc_histoSize,
                           const std::vector<std::vector<int32_t> > acc_uramHisto,
                           const std::vector<std::vector<int32_t> > acc_hbmHisto,
                           const std::vector<uint32_t> acc_totalcount,
                           const std::vector<float> acc_entropy,
                           std::vector<uint32_t> nonempty_histograms,

                           uint32_t refSize,
                           std::vector<int32_t> ref_histo,
                           uint32_t ref_totalcount,
                           float ref_entropy,

                           std::vector<float>& dists,
                           std::vector<uint32_t>& best, 
                           uint32_t& largest_idx
#else
                           bool isEntropy, uint32_t num_contexts, uint32_t j,

                           uint32_t acc_histoSize[8192],
                           int32_t acc_uramHisto[4096][40],
                           int32_t acc_hbmHisto[4096][40],
                           uint32_t acc_totalcount[8192],
                           float acc_entropy[8192],
                           uint32_t nonempty_histograms[8192],

                           uint32_t refSize,
                           int32_t ref_histo[40],
                           uint32_t ref_totalcount,
                           float ref_entropy,

                           float dists[1024],
                           uint32_t best[1024], 
                           uint32_t& largest_idx
#endif
) {
    // clang-format on
    largest_idx = 0;
    for (size_t i = 0; i < num_contexts; i++) {
        int idx = nonempty_histograms[i];
        std::vector<int32_t> tmp_histo = idx < 4096 ? acc_uramHisto[idx] : acc_hbmHisto[idx - 4096];
        float dist_std = accHistogramDistanceEntropy(isEntropy, acc_histoSize[idx], acc_totalcount[idx], tmp_histo,
                                                     refSize, ref_totalcount, ref_histo);
        if (!isEntropy) {
            if (dist_std - acc_entropy[i] - ref_entropy < dists[i]) {
                best[i] = j;
                dists[i] = dist_std - acc_entropy[i] - ref_entropy;
            }
        } else {
            dists[i] = dist_std;
        }
        if (dists[i] > dists[largest_idx]) largest_idx = i;
    }
}

void FastClusterHistograms(const std::vector<Histogram>& in,
                           const size_t num_contexts_in,
                           size_t max_histograms,
                           float min_distance,
                           std::vector<Histogram>* out,
                           std::vector<uint32_t>* histogram_symbols) {
    PROFILER_FUNC;
    uint32_t largest_idx = 0;
    std::vector<uint32_t> nonempty_histograms;
    nonempty_histograms.reserve(in.size());
    for (size_t i = 0; i < num_contexts_in; i++) {
        if (in[i].total_count_ == 0) continue;

        if (in[i].total_count_ > in[largest_idx].total_count_) {
            largest_idx = i;
        }
        nonempty_histograms.push_back(i);
    }

    largest_idx =
        std::find(nonempty_histograms.begin(), nonempty_histograms.end(), largest_idx) - nonempty_histograms.begin();

    size_t num_contexts = nonempty_histograms.size();
    std::vector<float> entropy(num_contexts);
    //  for(size_t i=0;i<num_contexts;i++){
    //    entropy[i]=accHistogramDistanceEntropy(in[nonempty_histograms[i]],in[nonempty_histograms[i]],true);
    //  }

    std::vector<std::vector<int32_t> > acc_uramHisto(4096, std::vector<int32_t>(40, 0));
    std::vector<std::vector<int32_t> > acc_hbmHisto(4096, std::vector<int32_t>(40, 0));
    std::vector<uint32_t> acc_total_count(8192, 0);
    std::vector<float> acc_entropy(8192, 0);
    std::vector<uint32_t> acc_histoSize(8192, 0);

    for (int i = 0; i < in.size(); i++) {
        acc_total_count[i] = in[i].total_count_;
        acc_entropy[i] = in[i].entropy_;
        acc_histoSize[i] = in[i].data_.size();
        for (int j = 0; j < in[i].data_.size(); j++) {
            if (i < 4096) {
                acc_uramHisto[i][j] = in[i].data_[j];
            } else if (i < 8192) {
                acc_hbmHisto[i - 4096][j] = in[i].data_[j];
            } else {
                std::cout << "Error Histogram too big!" << std::endl;
            }
        }
    }

    Histogram tmp0;
    std::vector<uint32_t> tmp1;
    uint32_t tmp2;
    acc_HistogramDistance(true, num_contexts, 0, acc_histoSize, acc_uramHisto, acc_hbmHisto, acc_total_count,
                          acc_entropy, nonempty_histograms, tmp0.data_.size(), tmp0.data_, tmp0.total_count_,
                          tmp0.entropy_, entropy, tmp1, tmp2);

    for (size_t i = 0; i < num_contexts; i++) {
        in[nonempty_histograms[i]].entropy_ = entropy[i];
        acc_entropy[nonempty_histograms[i]] = entropy[i];
    }

    // No symbols.
    if (nonempty_histograms.empty()) {
        out->resize(1);
        histogram_symbols->clear();
        histogram_symbols->resize(in.size(), 0);
        return;
    }

    out->clear();
    out->reserve(max_histograms);
    std::vector<float> dists(num_contexts, std::numeric_limits<float>::max());
    std::vector<uint32_t> best_tmp(num_contexts, 0); // no use
    histogram_symbols->clear();
    histogram_symbols->resize(in.size(), 0);

    while (out->size() < max_histograms && out->size() < num_contexts) {
        (*histogram_symbols)[nonempty_histograms[largest_idx]] = out->size();
        out->push_back(in[nonempty_histograms[largest_idx]]);
        Histogram backhisto = out->back();
        acc_HistogramDistance(false, num_contexts, 0, acc_histoSize, acc_uramHisto, acc_hbmHisto, acc_total_count,
                              entropy, nonempty_histograms, backhisto.data_.size(), backhisto.data_,
                              backhisto.total_count_, backhisto.entropy_, dists, best_tmp, largest_idx);
        if (dists[largest_idx] < min_distance) break;
    }

    std::vector<float> best_dist(num_contexts, std::numeric_limits<float>::max());
    std::vector<uint32_t> best(num_contexts, 0);

    for (size_t j = 0; j < out->size(); j++) {
        Histogram outHisto = (*out)[j];
        acc_HistogramDistance(false, num_contexts, j, acc_histoSize, acc_uramHisto, acc_hbmHisto, acc_total_count,
                              entropy, nonempty_histograms, outHisto.data_.size(), outHisto.data_,
                              outHisto.total_count_, outHisto.entropy_, best_dist, best, largest_idx);
    }

    for (size_t i = 0; i < num_contexts; i++) {
        for (size_t j = 0; j < out->size(); j++) {
            (*out)[best[i]].AddHistogram(in[nonempty_histograms[i]]);
            (*histogram_symbols)[nonempty_histograms[i]] = best[i];
        }
    }
}

// NOLINTNEXTLINE(google-readability-namespace-comments)
} // namespace HWY_NAMESPACE
} // namespace jxl
HWY_AFTER_NAMESPACE();

#if HWY_ONCE
namespace jxl {
HWY_EXPORT(FastClusterHistograms); // Local function
HWY_EXPORT(HistogramEntropy);      // Local function

float Histogram::ShannonEntropy() const {
    HWY_DYNAMIC_DISPATCH(HistogramEntropy)(*this);
    return entropy_;
}

// Reorder histograms in *out so that the new symbols in *symbols come in
// increasing order.
void HistogramReindex(std::vector<Histogram>* out, std::vector<uint32_t>* symbols) {
    std::vector<Histogram> tmp(*out);
    std::map<int, int> new_index;
    int next_index = 0;
    for (uint32_t symbol : *symbols) {
        if (new_index.find(symbol) == new_index.end()) {
            new_index[symbol] = next_index;
            (*out)[next_index] = tmp[symbol];
            ++next_index;
        }
    }
    out->resize(next_index);
    for (uint32_t& symbol : *symbols) {
        symbol = new_index[symbol];
    }
}

// Clusters similar histograms in 'in' together, the selected histograms are
// placed in 'out', and for each index in 'in', *histogram_symbols will
// indicate which of the 'out' histograms is the best approximation.
void ClusterHistograms(const HistogramParams params,
                       const std::vector<Histogram>& in,
                       const size_t num_contexts,
                       size_t max_histograms,
                       std::vector<Histogram>* out,
                       std::vector<uint32_t>* histogram_symbols) {
    constexpr float kMinDistanceForDistinctFast = 64.0f;
    constexpr float kMinDistanceForDistinctBest = 16.0f;
    max_histograms = std::min(max_histograms, params.max_histograms);
    // printf("%s: %s: %d, max_histograms=%d\n", __FILE__, __FUNCTION__, __LINE__,
    //       max_histograms);
    if (params.clustering == HistogramParams::ClusteringType::kFastest) {
        HWY_DYNAMIC_DISPATCH(FastClusterHistograms)
        (in, num_contexts, 4, kMinDistanceForDistinctFast, out, histogram_symbols);
    } else if (params.clustering == HistogramParams::ClusteringType::kFast) {
        HWY_DYNAMIC_DISPATCH(FastClusterHistograms)
        (in, num_contexts, max_histograms, kMinDistanceForDistinctFast, out, histogram_symbols);
    } else {
        PROFILER_FUNC;
        HWY_DYNAMIC_DISPATCH(FastClusterHistograms)
        (in, num_contexts, max_histograms, kMinDistanceForDistinctBest, out, histogram_symbols);

        // printf("%s: %s: %d, FastClusterHistograms out->size=%d\n", __FILE__,
        //       __FUNCTION__, __LINE__, out->size());
        for (size_t i = 0; i < out->size(); i++) {
            (*out)[i].entropy_ = ANSPopulationCost((*out)[i].data_.data(), (*out)[i].data_.size());
        }
        uint32_t next_version = 2;
        std::vector<uint32_t> version(out->size(), 1);
        std::vector<uint32_t> renumbering(out->size());
        std::iota(renumbering.begin(), renumbering.end(), 0);

        // Try to pair up clusters if doing so reduces the total cost.

        struct HistogramPair {
            // validity of a pair: p.version == max(version[i], version[j])
            float cost;
            uint32_t first;
            uint32_t second;
            uint32_t version;
            // We use > because priority queues sort in *decreasing* order, but we
            // want lower cost elements to appear first.
            bool operator<(const HistogramPair& other) const {
                return std::make_tuple(cost, first, second, version) >
                       std::make_tuple(other.cost, other.first, other.second, other.version);
            }
        };

        // Create list of all pairs by increasing merging cost.
        std::priority_queue<HistogramPair> pairs_to_merge;
        for (uint32_t i = 0; i < out->size(); i++) {
            for (uint32_t j = i + 1; j < out->size(); j++) {
                Histogram histo;
                histo.AddHistogram((*out)[i]);
                histo.AddHistogram((*out)[j]);
                float cost =
                    ANSPopulationCost(histo.data_.data(), histo.data_.size()) - (*out)[i].entropy_ - (*out)[j].entropy_;
                // Avoid enqueueing pairs that are not advantageous to merge.
                if (cost >= 0) continue;
                pairs_to_merge.push(HistogramPair{cost, i, j, std::max(version[i], version[j])});
            }
        }

        int merge_count = 0;
        // Merge the best pair to merge, add new pairs that get formed as a
        // consequence.
        while (!pairs_to_merge.empty()) {
            merge_count++;
            uint32_t first = pairs_to_merge.top().first;
            uint32_t second = pairs_to_merge.top().second;
            uint32_t ver = pairs_to_merge.top().version;
            pairs_to_merge.pop();
            if (ver != std::max(version[first], version[second]) || version[first] == 0 || version[second] == 0) {
                continue;
            }
            (*out)[first].AddHistogram((*out)[second]);
            (*out)[first].entropy_ = ANSPopulationCost((*out)[first].data_.data(), (*out)[first].data_.size());
            for (size_t i = 0; i < renumbering.size(); i++) {
                if (renumbering[i] == second) {
                    renumbering[i] = first;
                }
            }
            version[second] = 0;
            version[first] = next_version++;
            for (uint32_t j = 0; j < out->size(); j++) {
                if (j == first) continue;
                if (version[j] == 0) continue;
                Histogram histo;
                histo.AddHistogram((*out)[first]);
                histo.AddHistogram((*out)[j]);
                float cost = ANSPopulationCost(histo.data_.data(), histo.data_.size()) - (*out)[first].entropy_ -
                             (*out)[j].entropy_;
                // Avoid enqueueing pairs that are not advantageous to merge.
                if (cost >= 0) continue;
                pairs_to_merge.push(
                    HistogramPair{cost, std::min(first, j), std::max(first, j), std::max(version[first], version[j])});
            }
        }
        std::vector<uint32_t> reverse_renumbering(out->size(), -1);
        size_t num_alive = 0;
        for (size_t i = 0; i < out->size(); i++) {
            if (version[i] == 0) continue;
            (*out)[num_alive++] = (*out)[i];
            reverse_renumbering[i] = num_alive - 1;
        }
        out->resize(num_alive);
        // printf(
        //    "%s: %s: %d, culster num_alive=%zu, histogram_symbols size=%zu, "
        //    "merge_count=%d\n",
        //    __FILE__, __FUNCTION__, __LINE__, num_alive,
        //    histogram_symbols->size(), merge_count);
        for (size_t i = 0; i < histogram_symbols->size(); i++) {
            (*histogram_symbols)[i] = reverse_renumbering[renumbering[(*histogram_symbols)[i]]];
        }
    }

    // Convert the context map to a canonical form.
    HistogramReindex(out, histogram_symbols);
    // printf("%s: %s: %d, culster final out size=%zu, histogram_symbols
    // size=%zu\n",
    //       __FILE__, __FUNCTION__, __LINE__, out->size(),
    //       histogram_symbols->size());
}

void acc_FastClusterHistograms(const std::vector<Histogram>& in,
                               std::vector<uint32_t> nonempty_histograms,
                               uint32_t largest_idx_in,
                               const size_t num_contexts,
                               size_t max_histograms,
                               float min_distance,
                               std::vector<Histogram>* out,
                               std::vector<uint32_t>* histogram_symbols) {
    PROFILER_FUNC;

    uint32_t largest_idx = largest_idx_in;
    std::vector<float> entropy(num_contexts);
    //  for(size_t i=0;i<num_contexts;i++){
    //    entropy[i]=accHistogramDistanceEntropy(in[nonempty_histograms[i]],in[nonempty_histograms[i]],true);
    //  }

    std::vector<std::vector<int32_t> > acc_uramHisto(4096, std::vector<int32_t>(40, 0));
    std::vector<std::vector<int32_t> > acc_hbmHisto(4096, std::vector<int32_t>(40, 0));
    std::vector<uint32_t> acc_total_count(8192, 0);
    std::vector<float> acc_entropy(8192, 0);
    std::vector<uint32_t> acc_histoSize(8192, 0);

    for (int i = 0; i < in.size(); i++) {
        acc_total_count[i] = in[i].total_count_;
        acc_entropy[i] = in[i].entropy_;
        acc_histoSize[i] = in[i].data_.size();
        for (int j = 0; j < in[i].data_.size(); j++) {
            if (i < 4096) {
                acc_uramHisto[i][j] = in[i].data_[j];
            } else if (i < 8192) {
                acc_hbmHisto[i - 4096][j] = in[i].data_[j];
            } else {
                std::cout << "Error Histogram too big!" << std::endl;
            }
        }
    }

    Histogram tmp0;
    std::vector<uint32_t> tmp1;
    uint32_t tmp2;
    jxl::N_SCALAR::acc_HistogramDistance(true, num_contexts, 0, acc_histoSize, acc_uramHisto, acc_hbmHisto,
                                         acc_total_count, acc_entropy, nonempty_histograms, tmp0.data_.size(),
                                         tmp0.data_, tmp0.total_count_, tmp0.entropy_, entropy, tmp1, tmp2);

    for (size_t i = 0; i < num_contexts; i++) {
        in[nonempty_histograms[i]].entropy_ = entropy[i];
        acc_entropy[nonempty_histograms[i]] = entropy[i];
    }

    // No symbols.
    if (nonempty_histograms.empty()) {
        out->resize(1);
        histogram_symbols->clear();
        histogram_symbols->resize(in.size(), 0);
        return;
    }

    out->clear();
    out->reserve(max_histograms);
    std::vector<float> dists(num_contexts, std::numeric_limits<float>::max());
    std::vector<uint32_t> best_tmp(num_contexts, 0); // no use
    histogram_symbols->clear();
    histogram_symbols->resize(in.size(), 0);

    while (out->size() < max_histograms && out->size() < num_contexts) {
        (*histogram_symbols)[nonempty_histograms[largest_idx]] = out->size();
        out->push_back(in[nonempty_histograms[largest_idx]]);
        Histogram backhisto = out->back();
        jxl::N_SCALAR::acc_HistogramDistance(false, num_contexts, 0, acc_histoSize, acc_uramHisto, acc_hbmHisto,
                                             acc_total_count, entropy, nonempty_histograms, backhisto.data_.size(),
                                             backhisto.data_, backhisto.total_count_, backhisto.entropy_, dists,
                                             best_tmp, largest_idx);
        if (dists[largest_idx] < min_distance) break;
    }

    std::vector<float> best_dist(num_contexts, std::numeric_limits<float>::max());
    std::vector<uint32_t> best(num_contexts, 0);

    for (size_t j = 0; j < out->size(); j++) {
        Histogram outHisto = (*out)[j];
        jxl::N_SCALAR::acc_HistogramDistance(false, num_contexts, j, acc_histoSize, acc_uramHisto, acc_hbmHisto,
                                             acc_total_count, entropy, nonempty_histograms, outHisto.data_.size(),
                                             outHisto.data_, outHisto.total_count_, outHisto.entropy_, best_dist, best,
                                             largest_idx);
    }

    for (size_t i = 0; i < num_contexts; i++) {
        for (size_t j = 0; j < out->size(); j++) {
            (*out)[best[i]].AddHistogram(in[nonempty_histograms[i]]);
            (*histogram_symbols)[nonempty_histograms[i]] = best[i];
        }
    }
}

void ClusterHistogramsNew(const HistogramParams params,
                          const std::vector<Histogram>& in,
                          const size_t num_contexts,
                          size_t max_histograms,
                          std::vector<Histogram>* out,
                          std::vector<uint32_t>* histogram_symbols) {
    constexpr float kMinDistanceForDistinctFast = 64.0f;
    constexpr float kMinDistanceForDistinctBest = 16.0f;
    max_histograms = std::min(max_histograms, params.max_histograms);
    // printf("%s: %s: %d, max_histograms=%d\n", __FILE__, __FUNCTION__, __LINE__,
    //       max_histograms);

    uint32_t largest_idx = 0;
    std::vector<uint32_t> nonempty_histograms;
    nonempty_histograms.reserve(in.size());
    for (size_t i = 0; i < num_contexts; i++) {
        if (in[i].total_count_ == 0) continue;

        if (in[i].total_count_ > in[largest_idx].total_count_) {
            largest_idx = i;
        }
        nonempty_histograms.push_back(i);
    }

    largest_idx =
        std::find(nonempty_histograms.begin(), nonempty_histograms.end(), largest_idx) - nonempty_histograms.begin();

    acc_FastClusterHistograms(in, nonempty_histograms, largest_idx, nonempty_histograms.size(), max_histograms,
                              kMinDistanceForDistinctFast, out, histogram_symbols);

    // Convert the context map to a canonical form.
    HistogramReindex(out, histogram_symbols);
    // printf("%s: %s: %d, culster final out size=%zu, histogram_symbols
    // size=%zu\n",
    //       __FILE__, __FUNCTION__, __LINE__, out->size(),
    //       histogram_symbols->size());
}

} // namespace jxl
#endif // HWY_ONCE
