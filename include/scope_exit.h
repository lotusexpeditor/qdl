#pragma once

#include <functional>

struct scope_exit {
	using Func = std::function<void()>;

	scope_exit(Func _f) : func(_f) {}
	~scope_exit() { func(); }

   private:
	Func& func;
};
