#!/bin/bash

# 強制跳轉到腳本所在資料夾的上一層（專案根目錄）
cd "$(dirname "$0")/.."

# 定義變數
IMAGE_NAME="taifex-resolver-img"
APP_CONTAINER="taifex-app"
MOCKER_CONTAINER="taifex-mocker"

echo "--- [1/4] 開始編譯 C++ 程式 (位置: $(pwd)) ---"

# --- 核心修正點 ---
# 1. 必須包含 src/parser.cc 才能找到函數實作
# 2. 必須加上 -lpthread 才能支援多執行緒
g++ -O3 -Wall -std=c++17 \
    -I ./include \
    example/taifex_resolver_interface.cc \
    src/parser.cc \
    -o example/taifex_resolver_app \
    -lpthread

# 檢查編譯是否成功
if [ $? -eq 0 ]; then
    echo "✅ 編譯成功！執行檔已產生：example/taifex_resolver_app"
else
    echo "❌ 錯誤：編譯失敗！"
    echo "請確認 src/parser.cc 是否存在，且內容沒有語法錯誤。"
    exit 1
fi

echo "--- [2/4] 正在停止並移除舊容器 ---"
docker rm -f $APP_CONTAINER $MOCKER_CONTAINER 2>/dev/null

echo "--- [3/4] 正在重新構建 Docker 鏡像 ---"
docker build -t $IMAGE_NAME .

echo "--- [4/4] 啟動容器 ---"
# 啟動 Resolver App
docker run -d \
  --name $APP_CONTAINER \
  --restart always \
  $IMAGE_NAME ./example/taifex_resolver_app -port 10000