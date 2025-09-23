#!/bin/bash
# Compile GLSL shaders to SPIR-V

# Ensure glslc compiler is available
if ! command -v glslc &> /dev/null; then
    echo "Error: glslc not found. Please install the Vulkan SDK."
    exit 1
fi

# Create spirv directory if it doesn't exist
mkdir -p shaders/spirv

# Compile compute shader
echo "Compiling keystone_compute.comp..."
glslc -o shaders/spirv/keystone_compute.spv shaders/keystone_compute.comp

echo "Compilation complete!"