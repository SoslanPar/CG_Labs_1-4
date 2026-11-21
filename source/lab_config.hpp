#pragma once
// Одно значение передаётся из CMake в C++ и GLSL.
#ifndef LAB_NUMBER
#define LAB_NUMBER 3
#endif
#if LAB_NUMBER == 1
#define LAB_WINDOW_TITLE "Lab 1 - Geometry and transformations"
#elif LAB_NUMBER == 2
#define LAB_WINDOW_TITLE "Lab 2 - Camera and lighting"
#elif LAB_NUMBER == 4
#define LAB_WINDOW_TITLE "Lab 4 - Dynamic shadow mapping"
#else
#define LAB_WINDOW_TITLE "Lab 3 - Texture mapping"
#endif
