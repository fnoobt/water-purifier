#!/usr/bin/env python3
import os
import subprocess
import sys

# 设置ESP-IDF环境
idf_path = r'D:\esp\v5.5.2\esp-idf'
tools_path = r'C:\Espressif\tools'

os.environ['IDF_PATH'] = idf_path
os.environ['PATH'] = (
    tools_path + r'\cmake\3.30.2\bin;' +
    tools_path + r'\ninja\1.11.1;' +
    tools_path + r'\riscv32-esp-elf\esp-14.2.0_20251107\riscv32-esp-elf\bin;' +
    os.environ.get('PATH', '')
)

# 切换到项目目录
os.chdir(r'D:\Projects\esp32\WaterPurifier')

# 清理旧的构建目录
import shutil
if os.path.exists('build'):
    shutil.rmtree('build')
os.makedirs('build')

# 运行CMake配置
print("配置CMake...")
cmake_cmd = [
    tools_path + r'\cmake\3.30.2\bin\cmake.exe',
    '-G', 'Ninja',
    '-B', 'build',
    '-DESP_PLATFORM=esp32c3',
    '-DIDF_PATH=' + idf_path,
    '-DPARTITION_TABLE_FILENAME=partitions_singleapp.csv',
    '.'
]
result = subprocess.run(cmake_cmd, capture_output=True, text=True)
print(result.stdout)
if result.returncode != 0:
    print("CMake配置失败:")
    print(result.stderr)
    sys.exit(1)

# 运行构建
print("构建项目...")
ninja_cmd = [tools_path + r'\ninja\1.11.1\ninja.exe', '-C', 'build']
result = subprocess.run(ninja_cmd, capture_output=True, text=True)
print(result.stdout)
if result.returncode != 0:
    print("构建失败:")
    print(result.stderr)
    sys.exit(1)

print("构建完成!")
