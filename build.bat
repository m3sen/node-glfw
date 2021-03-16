echo %0 %1 %2 %3

cd %2
cd deps
cd glfw-3.0.4
if exist CMakeCache.txt del /f /q CMakeCache.txt

IF /I "%1"=="x64" (
	cmake -DCMAKE_WARN_DEPRECATED=0 -DBUILD_SHARED_LIBS=1 -DGLFW_BUILD_EXAMPLES=0 -DGLFW_BUILD_TESTS=0 -DGLFW_BUILD_DOCS=0 -DCMAKE_GENERATOR_PLATFORM=x64 . > NUL
) else (
	cmake -DCMAKE_WARN_DEPRECATED=0 -DBUILD_SHARED_LIBS=1 -DGLFW_BUILD_EXAMPLES=0 -DGLFW_BUILD_TESTS=0 -DGLFW_BUILD_DOCS=0 . > NUL
)

IF errorlevel 1 (
	echo "GLFW cmake failed"
	exit 2
)

msbuild GLFW.sln /p:Configuration=Release /p:Platform=%3 /m:4

IF errorlevel 1 (
	echo "GLFW Visual Stuio build (msbuild) failed"
	exit 3
) else (
	echo "Success: GLFW is built"
	exit 0
)
