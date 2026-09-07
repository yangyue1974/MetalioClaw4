#pragma once
struct IOExpander { enum class Pin { BT_POWER }; static IOExpander& getInstance(){ static IOExpander i; return i; } void setLevel(Pin,bool){} };
