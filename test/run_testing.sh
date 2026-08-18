#!/bin/bash

# 1. Path setup
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$PROJECT_ROOT"

# Variables
SESSION_NAME="taifex_dev"
IMAGE_NAME="taifex-resolver-img"
CONTAINER_NAME="taifex-app"

echo "--- [1/4] Cleaning up old environment ---"
# Kill old tmux session if exists
tmux kill-session -t $SESSION_NAME 2>/dev/null
# Force remove old container to avoid name conflict
docker rm -f $CONTAINER_NAME 2>/dev/null

echo "--- [2/4] Compiling with CMake ---"
mkdir -p build && cd build
cmake ..
make -j$(nproc)
if [ $? -ne 0 ]; then
    echo "❌ Compilation failed."
    exit 1
fi
cd "$PROJECT_ROOT"

echo "--- [3/4] Building Docker Image ---"
docker build -f dockerfile -t $IMAGE_NAME .

echo "--- [4/4] Starting tmux environment ---"
tmux new-session -d -s $SESSION_NAME

# Split window to the left
tmux split-window -h -t $SESSION_NAME


# Right window: Run the NEW image with --network host
# We use the built binary inside the container
tmux send-keys -t $SESSION_NAME "docker run -d --rm --network host --name $CONTAINER_NAME $IMAGE_NAME ./build/taifex_resolver_cpp && sleep 1 \
    && docker exec -it $CONTAINER_NAME tail -f logger/taifex_parser.log" C-m

# Left window: Prepare Mocker command
tmux send-keys -t $SESSION_NAME:0.0 "python3 test/TAIFEX_mocker.py" C-m

# Attach to session
tmux attach-session -t $SESSION_NAME