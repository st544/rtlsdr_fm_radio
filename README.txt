Local Build instructions

1. Launch VS Code from x64 Native Tools Command Prompt for VS 2019 to activate MSVC compiler environment
- Open x64 Native Tools Command Prompt for VS Code 2019 from Start menu
- Navivate to project directory
- Type code . to launch 

2. Check required files
- vcpkg.json
- CMakePresets.json
- CMakeLists.txt

3. Build with presets (first time only)
In project directory:
cmake --preset win-release
cmake --build build/win-release

4. Building subsequent executables:
cmake --build .
OR
ninja
inside build/win-release