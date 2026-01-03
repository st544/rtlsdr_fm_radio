Build instructions

1. Launch VS Code from Developer Powershell to activate MSVC compiler environment
- Open Developer PowerShell for VS Code 2019 from Start menu
- Navivate to project directory
- Type code . to launch 

2. Check required files
- vcpkg.json
- CMakePresets.json
- CMakeLists.txt

3. Build with presets (first time only)
In project directory:
cmake --preset win-debug
cmake --build build/win-debug

4. Building subsequent executables:
cmake --build ..