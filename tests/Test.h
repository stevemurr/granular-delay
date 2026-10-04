#pragma once
#include <cstdlib>
#include <iostream>
#define CHECK(condition) do { if (!(condition)) { std::cerr << __FILE__ << ':' << __LINE__ << ": " << #condition << '\n'; std::exit(1); } } while (false)

