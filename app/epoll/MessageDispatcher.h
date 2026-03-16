#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>
#include <unordered_map>

class TcpConnection;

class MessageDispatcher {
public:
	using ConnectionPtr = std::shared_ptr<TcpConnection>;
	using Callback = std::function<void(uint32_t code, std::string_view payload, const ConnectionPtr& conn)>;

	void registerHandler(uint32_t code, Callback cb) { handlers_[code] = std::move(cb); }

	void setDefaultHandler(Callback cb) { defaultHandler_ = std::move(cb); }

	bool dispatch(uint32_t code, std::string_view payload, const ConnectionPtr& conn) const {
		const auto it = handlers_.find(code);
		if (it != handlers_.end()) {
			it->second(code, payload, conn);
			return true;
		}

		if (defaultHandler_) {
			defaultHandler_(code, payload, conn);
		}
		return false;
	}

private:
	std::unordered_map<uint32_t, Callback> handlers_;
	Callback defaultHandler_;
};
