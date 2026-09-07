#pragma once
#include <functional>
#include <vector>
#include <string>
#include <cstdint>
struct SimpleUart { static SimpleUart& getInstance(){ static SimpleUart u; return u; }
 void registerCallback(std::function<void(const std::vector<uint8_t>&)> c){cb=c;} bool sendString(const char*){return true;} bool isInitialized() const {return true;}
 std::function<void(const std::vector<uint8_t>&)> cb; };
