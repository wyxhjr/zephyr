cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(riscv_aia)

# Add test source files
target_sources(app PRIVATE src/main.c)

# Include paths for AIA headers
target_include_directories(app PRIVATE
    ${ZEPHYR_BASE}/include
    ${ZEPHYR_BASE}/include/zephyr
    ${ZEPHYR_BASE}/include/zephyr/drivers
)


