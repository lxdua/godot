#!/usr/bin/env bash
# DuaDot Engine automation common library
# shellcheck disable=SC2034,SC2155

set -euo pipefail

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

# Paths
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
CONFIG_FILE="${PROJECT_ROOT}/scripts/config.sh"

# Defaults
UPSTREAM_REMOTE="${UPSTREAM_REMOTE:-upstream}"
UPSTREAM_URL="${UPSTREAM_URL:-https://github.com/godotengine/godot.git}"
MASTER_BRANCH="${MASTER_BRANCH:-master}"
DEVELOP_BRANCH="${DEVELOP_BRANCH:-develop}"
RELEASE_PREFIX="${RELEASE_PREFIX:-release/}"
JOBS="${JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)}"

# Load user config if exists
if [[ -f "$CONFIG_FILE" ]]; then
    # shellcheck source=/dev/null
    source "$CONFIG_FILE"
fi

# Logging functions
log_info()  { echo -e "${BLUE}[INFO]${NC}  $*"; }
log_ok()    { echo -e "${GREEN}[OK]${NC}    $*"; }
log_warn()  { echo -e "${YELLOW}[WARN]${NC}  $*"; }
log_err()   { echo -e "${RED}[ERROR]${NC} $*" >&2; }
log_step()  { echo -e "${CYAN}▶ $*${NC}"; }

# Ensure we are in the project root
cd_project_root() {
    cd "$PROJECT_ROOT"
}

# Check if a git remote exists
git_remote_exists() {
    git remote get-url "$1" >/dev/null 2>&1
}

# Ensure upstream remote is configured
ensure_upstream() {
    cd_project_root
    if ! git_remote_exists "$UPSTREAM_REMOTE"; then
        log_info "Adding upstream remote: $UPSTREAM_URL"
        git remote add "$UPSTREAM_REMOTE" "$UPSTREAM_URL"
    fi
    log_info "Fetching upstream..."
    git fetch "$UPSTREAM_REMOTE"
}

# Check working tree is clean
git_require_clean() {
    if ! git diff-index --quiet HEAD --; then
        log_err "Working tree is not clean. Commit or stash your changes first."
        git status --short
        exit 1
    fi
}

# Check branch exists
git_branch_exists() {
    git show-ref --verify --quiet "refs/heads/$1"
}

# Prompt yes/no
confirm() {
    local prompt="$1"
    read -r -p "$prompt [y/N] " response
    [[ "$response" =~ ^[Yy]$ ]]
}

# Check command exists
require_cmd() {
    if ! command -v "$1" >/dev/null 2>&1; then
        log_err "Required command not found: $1"
        exit 1
    fi
}

# Print banner
print_banner() {
    echo ""
    echo -e "${CYAN}╔══════════════════════════════════════╗${NC}"
    echo -e "${CYAN}║      DuaDot Engine Automation        ║${NC}"
    echo -e "${CYAN}╚══════════════════════════════════════╝${NC}"
    echo ""
}
