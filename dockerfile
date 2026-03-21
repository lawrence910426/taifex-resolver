# Use an official Ubuntu base image
FROM ubuntu:24.04

# Set environment variables
ENV DEBIAN_FRONTEND=noninteractive

# Install necessary packages
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    python3 \
    python3-dev \
    python3-pip \
    python3-venv \
    netcat-openbsd \
    tcpdump && \
    apt-get clean && rm -rf /var/lib/apt/lists/*

# Set the working directory
WORKDIR /app

# 1. [關鍵] 複製所有必要的原始碼與 CMake 設定檔
COPY ./src ./src
COPY ./include ./include
COPY ./example ./example
COPY ./CMakeLists.txt ./CMakeLists.txt
COPY ./test ./test

# 2. [關鍵] 在 Docker 內部執行編譯
# 這樣編譯出來的執行檔才會存在於容器的 /app/build 目錄下
RUN mkdir build && cd build && \
    cmake .. && \
    make -j$(nproc)

# 3. 安裝 Python 環境 (維持你原本的 uv 設定)
RUN pip install uv --break-system-packages
RUN uv venv /opt/venv
ENV PATH="/opt/venv/bin:$PATH"

# 如果你有 Python 專案需要安裝，取消下面註解
# COPY setup.py .
# RUN uv pip install .

# 賦予執行權限
RUN chmod +x /app/build/taifex_resolver_cpp