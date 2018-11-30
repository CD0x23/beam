// Copyright 2018 The Beam Team
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "api.h"

#include "nlohmann/json.hpp"

using json = nlohmann::json;

namespace beam
{
    struct jsonrpc_exception
    {
        int code;
        std::string message;
        int id;
    };

    void throwInvalidJsonRpc(int id = 0)
    {
        throw jsonrpc_exception{ INVALID_JSON_RPC , "Invalid JSON-RPC.", id };
    }

    void throwUnknownJsonRpc(int id)
    {
        throw jsonrpc_exception{ NOTFOUND_JSON_RPC , "Procedure not found.", id};
    }

    std::string getJsonString(const char* data, size_t size)
    {
        return std::string(data, data + (size > 1024 ? 1024 : size));
    }

    WalletApi::WalletApi(IWalletApiHandler& handler)
        : _handler(handler)
    {
        _methods["create_address"] = BIND_THIS_MEMFN(createAddressMethod);
        _methods["balance"] = BIND_THIS_MEMFN(balanceMethod);
    };


    void WalletApi::createAddressMethod(const nlohmann::json& msg)
    {
        LOG_DEBUG() << "createAddressMethod()";

        auto params = msg["params"];

        CreateAddress createAddress;

        createAddress.metadata = params["metadata"];

        _handler.onMessage(msg["id"], createAddress);
    }

    void WalletApi::balanceMethod(const nlohmann::json& msg)
    {
        LOG_DEBUG() << "balanceMethod()";

        auto params = msg["params"];

        Balance balance;
        balance.type = params["type"];
        balance.address.FromHex(params["addr"]);

        _handler.onMessage(msg["id"], balance);
    }

    void getResponse(int id, const CreateAddress::Response& data, json& msg)
    {
        msg = json
        {
            {"jsonrpc", "2.0"},
            {"id", id},
            {"result", std::to_string(data.address)}
        };
    }

    void getResponse(int id, const Balance::Response& data, json& msg)
    {
        msg = json
        {
            {"jsonrpc", "2.0"},
            {"id", id},
            {"result", data.amount}
        };
    }

    void WalletApi::getBalanceResponse(int id, const Amount& amount, json& msg)
    {
        msg = json
        {
            {"jsonrpc", "2.0"},
            {"id", id},
            {"result", amount}
        };
    }

    void WalletApi::getCreateAddressResponse(int id, const WalletID& address, json& msg)
    {
        msg = json
        {
            {"jsonrpc", "2.0"},
            {"id", id},
            {"result", std::to_string(address)}
        };
    }

    bool WalletApi::parse(const char* data, size_t size)
    {
        if (size == 0) return false;

        try
        {
            json msg = json::parse(data, data + size);

            if (msg["jsonrpc"] != "2.0") throwInvalidJsonRpc();
            if (msg["id"] <= 0) throwInvalidJsonRpc();
            if (msg["method"] == nullptr) throwInvalidJsonRpc();
            if (msg["params"] == nullptr) throwInvalidJsonRpc();
            if (_methods.find(msg["method"]) == _methods.end()) throwUnknownJsonRpc(msg["id"]);

            try
            {
                _methods[msg["method"]](msg);
            }
            catch (const nlohmann::detail::exception& e)
            {
                LOG_ERROR() << "json parse: " << e.what() << "\n" << getJsonString(data, size);

                throwInvalidJsonRpc(msg["id"]);
            }
        }
        catch (const jsonrpc_exception& e)
        {
            json msg
            {
                {"jsonrpc", "2.0"},
                {"error",
                    {
                        {"code", e.code},
                        {"message", e.message},
                    }
                }
            };

            if (e.id) msg["id"] = e.id;
            else msg["id"] = nullptr;

            _handler.onInvalidJsonRpc(msg);
        }
        catch (const std::exception& e)
        {
            LOG_ERROR() << "json parse: " << e.what() << "\n" << getJsonString(data, size);
            return false;
        }

        return true;
    }
}
