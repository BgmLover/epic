// Copyright (c) 2019 EPI-ONE Core Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "rpc_client.h"
#include "spdlog/spdlog.h"

rpc::Hash* HashToRPCHash(std::string h) {
    rpc::Hash* rpch = new rpc::Hash();
    rpch->set_hash(h);
    return rpch;
}

RPCClient::RPCClient(std::shared_ptr<grpc::Channel> channel)
    : be_stub_(BasicBlockExplorerRPC::NewStub(channel)), commander_stub_(CommanderRPC::NewStub(channel)) {}

std::optional<rpc::Block> RPCClient::GetBlock(std::string block_hash) {
    GetBlockRequest request;
    rpc::Hash* h = HashToRPCHash(block_hash);
    request.set_allocated_hash(h);

    GetBlockResponse reply;
    grpc::ClientContext context;
    grpc::Status status = be_stub_->GetBlock(&context, request, &reply);

    if (!status.ok()) {
        spdlog::error("No response from RPC server: {}", status.error_message());
        return {};
    }
    return {reply.block()};
}

std::optional<std::vector<rpc::Block>> RPCClient::GetLevelSet(std::string block_hash) {
    GetLevelSetRequest request;
    rpc::Hash* h = HashToRPCHash(block_hash);
    request.set_allocated_hash(h);

    GetLevelSetResponse reply;
    grpc::ClientContext context;
    grpc::Status status = be_stub_->GetLevelSet(&context, request, &reply);
    if (!status.ok()) {
        spdlog::error("No response from RPC server: {}", status.error_message());
        return {};
    }
    if (auto n = reply.blocks_size()) {
        std::vector<rpc::Block> r;
        r.resize(n);
        for (size_t i = 0; i < n; ++i) {
            r[i] = reply.blocks(i);
        }
        return {r};
    }
    return {};
}

std::optional<size_t> RPCClient::GetLevelSetSize(std::string block_hash) {
    GetLevelSetSizeRequest request;
    rpc::Hash* h = HashToRPCHash(block_hash);
    request.set_allocated_hash(h);

    GetLevelSetSizeResponse reply;
    grpc::ClientContext context;
    grpc::Status status = be_stub_->GetLevelSetSize(&context, request, &reply);
    if (!status.ok()) {
        spdlog::error("No response from RPC server: {}", status.error_message());
        return {};
    }
    return {reply.size()};
}

std::optional<rpc::Block> RPCClient::GetLatestMilestone() {
    GetLatestMilestoneRequest request;
    GetLatestMilestoneResponse reply;
    grpc::ClientContext context;
    grpc::Status status = be_stub_->GetLatestMilestone(&context, request, &reply);
    if (!status.ok()) {
        spdlog::error("No response from RPC server: {}", status.error_message());
        return {};
    }
    return {reply.milestone()};
}

std::optional<std::vector<rpc::Block>> RPCClient::GetNewMilestoneSince(std::string block_hash,
                                                                       size_t numberOfMilestone) {
    GetNewMilestoneSinceRequest request;
    rpc::Hash* h = HashToRPCHash(block_hash);
    request.set_allocated_hash(h);
    request.set_number(numberOfMilestone);

    GetNewMilestoneSinceResponse reply;
    grpc::ClientContext context;
    grpc::Status status = be_stub_->GetNewMilestoneSince(&context, request, &reply);
    if (!status.ok()) {
        spdlog::error("No response from RPC server: {}", status.error_message());
        return {};
    }

    if (auto n = reply.blocks_size()) {
        std::vector<rpc::Block> blocks;
        blocks.resize(n);
        for (size_t i = 0; i < n; ++i) {
            blocks[i] = reply.blocks(i);
        }
        return {blocks};
    }
    return {};
}

std::optional<StatusResponse> RPCClient::Status() {
    StatusRequest request;
    StatusResponse reply;
    grpc::ClientContext context;
    grpc::Status status = commander_stub_->Status(&context, request, &reply);
    if (!status.ok()) {
        spdlog::error("No response from RPC server: {}", status.error_message());
        return {};
    }
    return {reply};
}

bool RPCClient::Stop() {
    StopRequest request;
    StopResponse reply;
    grpc::ClientContext context;
    grpc::Status status = commander_stub_->Stop(&context, request, &reply);
    if (!status.ok()) {
        spdlog::error("No response from RPC server: {}", status.error_message());
        return false;
    }
    return true;
}

std::optional<bool> RPCClient::StartMiner() {
    StartMinerRequest request;
    StartMinerResponse reply;
    grpc::ClientContext context;
    grpc::Status status = commander_stub_->StartMiner(&context, request, &reply);
    if (!status.ok()) {
        spdlog::error("No response from RPC server: {}", status.error_message());
        return {};
    }
    return reply.success();
}

std::optional<bool> RPCClient::StopMiner() {
    StopMinerRequest request;
    StopMinerResponse reply;
    grpc::ClientContext context;
    grpc::Status status = commander_stub_->StopMiner(&context, request, &reply);
    if (!status.ok()) {
        spdlog::error("No response from RPC server: {}", status.error_message());
        return {};
    }
    return reply.success();
}

std::optional<std::string> RPCClient::CreateRandomTx(size_t size) {
    CreateRandomTxRequest request;
    CreateRandomTxResponse response;
    grpc::ClientContext context;
    request.set_size(size);
    auto status = commander_stub_->CreateRandomTx(&context, request, &response);
    if (!status.ok()) {
        spdlog::error("No response from RPC server: {}", status.error_message());
        return {};
    }
    return response.result();
}

RPCClient::option_string RPCClient::CreateTx(const std::vector<std::pair<uint64_t, std::string>>& outpus,
                                             uint64_t fee) {
    CreateTxRequest request;
    CreateTxResponse response;
    grpc::ClientContext context;

    request.set_fee(fee);
    for (auto& output : outpus) {
        auto rpc_output = request.add_outputs();
        rpc_output->set_address(output.second);
        rpc_output->set_money(output.first);
    }
    auto status = commander_stub_->CreateTx(&context, request, &response);
    if (!status.ok()) {
        spdlog::error("No response from RPC server: {}", status.error_message());
        return {};
    }
    return response.txinfo();
}

RPCClient::option_string RPCClient::GetBalance() {
    GetBalanceRequest request;
    GetBalanceResponse response;
    grpc::ClientContext context;

    auto status = commander_stub_->GetBalance(&context, request, &response);
    if (!status.ok()) {
        spdlog::error("No response from RPC server: {}", status.error_message());
        return {};
    }
    return response.coin();
}

RPCClient::option_string RPCClient::GenerateNewKey() {
    GenerateNewKeyRequest request;
    GenerateNewKeyResponse response;
    grpc::ClientContext context;

    auto status = commander_stub_->GenerateNewKey(&context, request, &response);
    if (!status.ok()) {
        spdlog::error("No response from RPC server: {}", status.error_message());
        return {};
    }

    return "Ckey = " + response.privatekey() + '\n' + "Address = " + response.address() + '\n';
}
