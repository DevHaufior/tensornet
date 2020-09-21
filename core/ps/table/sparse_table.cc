// Copyright (c) 2020, Qihoo, Inc.  All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//       http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "core/ps/table/sparse_table.h"

#include <set>
#include <string>

#include <butil/containers/flat_map.h>
#include <butil/logging.h>
#include <butil/object_pool.h>

#include "core/ps/optimizer/optimizer_kernel.h"

namespace tensornet {

SparseTable::SparseTable(const OptimizerBase* opt, int dimension,
        int shard_num, int self_shard_id)
    : shard_num_(shard_num)
    , self_shard_id_(self_shard_id)
    , opt_(opt)
    , dim_(dimension) {
    CHECK(opt_ != nullptr);

    op_kernel_ = opt_->CreateSparseOptKernel(dim_);

    grad_push_thread_ = std::thread(&SparseTable::UpdateGrad_, this);
}

SparseTable::~SparseTable() {
    stop_thread_ = true;
    grad_push_thread_.join();
}

void SparseTable::SetHandle(uint32_t handle) {
    CHECK(handle_ == 0) << "sparse table handle has already set:" << handle_;

    handle_ = handle;
}

void SparseTable::Pull(const SparsePullRequest* req, SparsePullResponse* resp) {
    resp->set_table_handle(req->table_handle());

    CHECK_EQ(dim_, req->dim());
    resp->set_dim(req->dim());

    for (int i = 0; i < req->signs_size(); ++i) {
        uint64_t sign = req->signs(i);

        float* w = op_kernel_->GetWeight(sign);
        CHECK(nullptr != w);

        auto weights = resp->add_weights();

        // w size is guaranteed by op_kernel_ same with dim_
        for (int j = 0; j < dim_; j++) {
            weights->add_w(w[j]);
        }
    }
}

void SparseTable::Push(const SparsePushRequest* req, SparsePushResponse* resp) {
    CHECK_EQ(dim_, req->dim());

    for (int i = 0; i < req->var_infos_size(); i++) {
        const auto& var_info = req->var_infos(i);

        CHECK_EQ(var_info.w_size(), dim_);

        SparseGradInfo grad_info;
        grad_info.sign = var_info.sign();
        grad_info.batch_show = var_info.batch_show();
        grad_info.grad = new float[dim_];

        for (int j = 0; j < var_info.w_size(); j++) {
            grad_info.grad[j] = var_info.w(j);
        }

        grad_update_queue_.push(std::move(grad_info));
    }
}

void SparseTable::Save(const std::string& filepath) const {
    butil::Timer timer(butil::Timer::STARTED);

    std::string file = filepath + "/sparse_table/" + std::to_string(GetHandle())
                             + "/rank_" + std::to_string(self_shard_id_);

    op_kernel_->Serialized(file);

    timer.stop();

    LOG(INFO) << "SparseTable save. rank:" << self_shard_id_
              << " table_id:" << GetHandle()
              << " latency:" << timer.s_elapsed() << "s"
              << " keys_count:" << op_kernel_->KeyCount();
}

void SparseTable::Load(const std::string& filepath) const {
    butil::Timer timer(butil::Timer::STARTED);

    std::string file = filepath + "/sparse_table/" + std::to_string(GetHandle())
                             + "/rank_" + std::to_string(self_shard_id_);
    op_kernel_->DeSerialized(file);

    timer.stop();

    LOG(INFO) << "SparseTable load. rank:" << self_shard_id_
              << " table_id:" << GetHandle()
              << " latency:" << timer.s_elapsed() << "s"
              << " keys_count:" << op_kernel_->KeyCount();
}

void SparseTable::ShowDecay() const {
    op_kernel_->ShowDecay();
}

void SparseTable::UpdateGrad_() {
    while (!stop_thread_) {
        SparseGradInfo grad_info;

        while (grad_update_queue_.pop(grad_info)) {
            op_kernel_->Apply(grad_info.sign, grad_info);
            delete grad_info.grad;
        }
    }
}

SparseTableRegistry* SparseTableRegistry::Instance() {
    static SparseTableRegistry instance;
    return &instance;
}

SparseTable* SparseTableRegistry::Get(uint32_t table_handle) {
    CHECK(table_handle < tables_.size())
        << " table_handle:" << table_handle << " table size:" << tables_.size();
    return tables_[table_handle];
}

uint32_t SparseTableRegistry::Register(SparseTable* table) {
    const std::lock_guard<std::mutex> lock(mu_);

    uint32_t table_handle = tables_.size();
    tables_.emplace_back(table);
    return table_handle;
}

SparseTable* CreateSparseTable(const OptimizerBase* opt, int dimension,
        int shard_num, int self_shard_id) {
    SparseTable* table = new SparseTable(opt, dimension, shard_num, self_shard_id);

    table->SetHandle(SparseTableRegistry::Instance()->Register(table));

    return table;
}

}  // namespace tensornet
