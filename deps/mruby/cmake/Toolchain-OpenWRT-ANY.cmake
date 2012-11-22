# Toolchain file for building with OpenWRT Toolchain for ANY OpenWRT Target.
# Following prequisition are necessary:
#   - latest cmake version
#   - mruby OpenWRT Package file (not yet in distribution)

# Switch to Cross Compile by setting the system name
SET(CMAKE_SYSTEM_NAME Linux)

# We show CMAKE the compiler, the rest will be guessed by the Toolchain
SET(CMAKE_C_COMPILER "$ENV{OPENWRT_TOOLCHAIN}/bin/$ENV{OPENWRT_TARGETCC}")

# We define an own release flag so that we can adapt the optimal C_FLAGS
SET(CMAKE_C_FLAGS_OPENWRT "$ENV{OPENWRT_TARGETFLAGS}")
