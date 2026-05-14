#!/usr/bin/env bash
# DuaDot Engine automation script
# Usage: ./scripts/dua.sh <command> [options]
#
# Commands:
#   sync     -- Sync upstream Godot into master and update develop
#   build    -- Build DuaDot Engine with SCons
#   template -- Build export templates
#   release  -- Manage release branches (init, cherry-pick, tag, status)
#   status   -- Show branch status and divergence overview
#
# Examples:
#   ./scripts/dua.sh sync
#   ./scripts/dua.sh build --target editor --dev
#   ./scripts/dua.sh template --platform windows
#   ./scripts/dua.sh release init 4.7-stable
#   ./scripts/dua.sh release cherry-pick a1b2c3d
#   ./scripts/dua.sh release tag v4.7.0-dua.1

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/lib/common.sh"

# ─── Helpers ───────────────────────────────────────────────────────

show_help() {
    cat <<'EOF'
DuaDot Engine Automation Script

Usage: ./scripts/dua.sh <command> [options]

Commands:
  sync [--rebase] [--merge]
    Sync upstream/master into local master, then update develop.
    Default strategy on master is merge; use --rebase for rebase.
    Develop is always rebased onto master.

  build [--target <name>] [--platform <name>] [--jobs N]
        [--dev] [--tests] [--clean] [--extra "flags"]
    Build with SCons. Defaults: target=editor, platform=auto, jobs=auto.
    --dev     shorthand for dev_mode=yes
    --tests   build with unit tests
    --clean   run scons -c first

  template [--platform <name>] [--jobs N] [--extra "flags"]
    Build both template_debug and template_release export templates.

  release init <name>
    Create a new release branch from develop.
    Example: release init 4.7-stable  →  branch release/4.7-stable

  release cherry-pick <commit-hash>
    Cherry-pick a commit from develop into the current release branch.

  release tag <version>
    Tag the current release branch. Example: release tag v4.7.0-dua.1

  release status
    Show commits in develop but not in current release branch.

  status
    Show status of master, develop, current branch vs upstream.

Examples:
  ./scripts/dua.sh sync
  ./scripts/dua.sh sync --rebase
  ./scripts/dua.sh build --target template_release --jobs 12
  ./scripts/dua.sh build --dev --tests --extra "module_mono_enabled=yes"
  ./scripts/dua.sh template --platform windows -j8
  ./scripts/dua.sh release init 4.7-stable
  ./scripts/dua.sh release cherry-pick 3f8a21b
  ./scripts/dua.sh release tag v4.7.0-dua.1
EOF
}

# ─── Sync Command ──────────────────────────────────────────────────

cmd_sync() {
    local strategy="merge"
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --rebase) strategy="rebase"; shift ;;
            --merge)  strategy="merge";  shift ;;
            *) log_err "Unknown sync option: $1"; exit 1 ;;
        esac
    done

    print_banner
    ensure_upstream
    git_require_clean

    cd_project_root
    local current_branch
    current_branch=$(git rev-parse --abbrev-ref HEAD)

    log_step "Updating ${MASTER_BRANCH} from upstream..."
    git checkout "$MASTER_BRANCH"
    if [[ "$strategy" == "rebase" ]]; then
        git rebase "${UPSTREAM_REMOTE}/${MASTER_BRANCH}"
    else
        git merge --ff-only "${UPSTREAM_REMOTE}/${MASTER_BRANCH}" 2>/dev/null || {
            log_warn "Fast-forward not possible. Running merge..."
            git merge "${UPSTREAM_REMOTE}/${MASTER_BRANCH}"
        }
    fi
    log_ok "${MASTER_BRANCH} is up to date with upstream."

    if git_branch_exists "$DEVELOP_BRANCH"; then
        log_step "Updating ${DEVELOP_BRANCH}..."
        git checkout "$DEVELOP_BRANCH"
        git rebase "$MASTER_BRANCH"
        log_ok "${DEVELOP_BRANCH} rebased onto ${MASTER_BRANCH}."
    else
        log_warn "Branch '${DEVELOP_BRANCH}' does not exist. Skipping."
    fi

    # Return to original branch if possible
    if [[ "$current_branch" != "$(git rev-parse --abbrev-ref HEAD)" ]]; then
        git checkout "$current_branch" && true || log_warn "Could not return to ${current_branch}"
    fi

    log_info "Sync complete."
}

# ─── Build Command ─────────────────────────────────────────────────

cmd_build() {
    local target="editor"
    local platform=""
    local jobs="$JOBS"
    local dev_mode=""
    local tests=""
    local clean=""
    local extra=""

    while [[ $# -gt 0 ]]; do
        case "$1" in
            --target)   target="$2"; shift 2 ;;
            --platform) platform="$2"; shift 2 ;;
            --jobs)     jobs="$2"; shift 2 ;;
            --dev)      dev_mode="yes"; shift ;;
            --tests)    tests="yes"; shift ;;
            --clean)    clean="yes"; shift ;;
            --extra)    extra="$2"; shift 2 ;;
            *) log_err "Unknown build option: $1"; exit 1 ;;
        esac
    done

    require_cmd scons
    cd_project_root

    if [[ "$clean" == "yes" ]]; then
        log_step "Cleaning build artifacts..."
        scons -c platform="${platform:-windows}" 2>/dev/null || true
    fi

    local scons_args=()
    [[ -n "$platform" ]] && scons_args+=("platform=$platform")
    scons_args+=("target=$target")
    scons_args+=("-j$jobs")
    [[ "$dev_mode" == "yes" ]] && scons_args+=("dev_mode=yes")
    [[ "$tests" == "yes" ]] && scons_args+=("tests=yes")
    [[ -n "$extra" ]] && scons_args+=("$extra")

    log_step "Building: scons ${scons_args[*]}"
    scons "${scons_args[@]}"
    log_ok "Build complete."
}

# ─── Template Command ──────────────────────────────────────────────

cmd_template() {
    local platform=""
    local jobs="$JOBS"
    local extra=""

    while [[ $# -gt 0 ]]; do
        case "$1" in
            --platform) platform="$2"; shift 2 ;;
            --jobs)     jobs="$2"; shift 2 ;;
            --extra)    extra="$2"; shift 2 ;;
            *) log_err "Unknown template option: $1"; exit 1 ;;
        esac
    done

    require_cmd scons
    cd_project_root

    local scons_args=()
    [[ -n "$platform" ]] && scons_args+=("platform=$platform")
    scons_args+=("-j$jobs")
    [[ -n "$extra" ]] && scons_args+=("$extra")

    log_step "Building export template (debug)..."
    scons "${scons_args[@]}" target=template_debug
    log_ok "Template debug build complete."

    log_step "Building export template (release)..."
    scons "${scons_args[@]}" target=template_release
    log_ok "Template release build complete."
}

# ─── Release Command ───────────────────────────────────────────────

cmd_release_init() {
    local name="${1:-}"
    if [[ -z "$name" ]]; then
        log_err "Release name required. Example: 4.7-stable"
        exit 1
    fi
    local branch="${RELEASE_PREFIX}${name}"

    cd_project_root
    git_require_clean

    if git_branch_exists "$branch"; then
        log_err "Branch '${branch}' already exists."
        exit 1
    fi

    if ! git_branch_exists "$DEVELOP_BRANCH"; then
        log_err "Develop branch '${DEVELOP_BRANCH}' does not exist."
        exit 1
    fi

    log_step "Creating release branch '${branch}' from '${DEVELOP_BRANCH}'..."
    git checkout "$DEVELOP_BRANCH"
    git pull origin "$DEVELOP_BRANCH" 2>/dev/null || true
    git checkout -b "$branch"
    log_ok "Release branch '${branch}' created."
    log_info "Next steps:"
    log_info "  1. Cherry-pick stable features: ./scripts/dua.sh release cherry-pick <commit>"
    log_info "  2. Remove experimental docs from dua_doc/"
    log_info "  3. Tag when ready: ./scripts/dua.sh release tag vX.Y.Z-dua.N"
}

cmd_release_cherrypick() {
    local hash="${1:-}"
    if [[ -z "$hash" ]]; then
        log_err "Commit hash required."
        exit 1
    fi

    cd_project_root
    git_require_clean

    local current_branch
    current_branch=$(git rev-parse --abbrev-ref HEAD)
    if [[ ! "$current_branch" =~ ^${RELEASE_PREFIX} ]]; then
        log_err "Current branch '${current_branch}' does not look like a release branch."
        log_err "Please checkout a '${RELEASE_PREFIX}*' branch first."
        exit 1
    fi

    log_step "Cherry-picking ${hash} into ${current_branch}..."
    if ! git cherry-pick "$hash"; then
        log_err "Cherry-pick failed. Resolve conflicts and run: git cherry-pick --continue"
        log_err "Or abort with: git cherry-pick --abort"
        exit 1
    fi
    log_ok "Cherry-pick complete."
}

cmd_release_tag() {
    local version="${1:-}"
    if [[ -z "$version" ]]; then
        log_err "Version tag required. Example: v4.7.0-dua.1"
        exit 1
    fi

    cd_project_root
    git_require_clean

    local current_branch
    current_branch=$(git rev-parse --abbrev-ref HEAD)
    if [[ ! "$current_branch" =~ ^${RELEASE_PREFIX} ]]; then
        log_err "Current branch '${current_branch}' does not look like a release branch."
        log_err "Please checkout a '${RELEASE_PREFIX}*' branch first."
        exit 1
    fi

    log_step "Tagging ${version} on ${current_branch}..."
    git tag -a "$version" -m "DuaDot Engine ${version}"
    log_ok "Tag ${version} created."
    log_info "Push with: git push origin ${current_branch} --follow-tags"
}

cmd_release_status() {
    cd_project_root
    local current_branch
    current_branch=$(git rev-parse --abbrev-ref HEAD)
    if [[ ! "$current_branch" =~ ^${RELEASE_PREFIX} ]]; then
        log_err "Current branch '${current_branch}' is not a release branch."
        exit 1
    fi

    log_info "Commits in '${DEVELOP_BRANCH}' but not in '${current_branch}':"
    git log --oneline --left-right "${current_branch}...${DEVELOP_BRANCH}" --right-only || true

    echo ""
    log_info "Commits in '${current_branch}' but not in '${DEVELOP_BRANCH}':"
    git log --oneline --left-right "${current_branch}...${DEVELOP_BRANCH}" --left-only || true
}

cmd_release() {
    local subcmd="${1:-}"
    shift || true
    case "$subcmd" in
        init)         cmd_release_init "$@" ;;
        cherry-pick)  cmd_release_cherrypick "$@" ;;
        tag)          cmd_release_tag "$@" ;;
        status)       cmd_release_status "$@" ;;
        *)
            log_err "Unknown release subcommand: ${subcmd:-(none)}"
            echo "Usage: release {init|cherry-pick|tag|status}"
            exit 1
            ;;
    esac
}

# ─── Status Command ────────────────────────────────────────────────

cmd_status() {
    cd_project_root
    print_banner

    log_info "Remotes:"
    git remote -v
    echo ""

    log_info "Local branches:"
    for branch in "$MASTER_BRANCH" "$DEVELOP_BRANCH"; do
        if git_branch_exists "$branch"; then
            local ahead_behind
            ahead_behind=$(git rev-list --left-right --count "${branch}...${UPSTREAM_REMOTE}/${MASTER_BRANCH}" 2>/dev/null || echo "? ?")
            printf "  %-20s %s\n" "$branch" "(vs upstream: ${ahead_behind})"
        else
            printf "  %-20s %s\n" "$branch" "(missing)"
        fi
    done

    for branch in $(git branch --list "${RELEASE_PREFIX}*" --format '%(refname:short)'); do
        local dev_diff
        dev_diff=$(git rev-list --count "${DEVELOP_BRANCH}...${branch}" 2>/dev/null || echo "?")
        printf "  %-20s %s\n" "$branch" "(${dev_diff} commits differ from ${DEVELOP_BRANCH})"
    done

    echo ""
    local current_branch
    current_branch=$(git rev-parse --abbrev-ref HEAD)
    log_info "Current branch: ${current_branch}"
    if ! git diff-index --quiet HEAD --; then
        log_warn "Working tree has uncommitted changes:"
        git status --short
    else
        log_ok "Working tree is clean."
    fi
}

# ─── Main ──────────────────────────────────────────────────────────

main() {
    if [[ $# -eq 0 ]]; then
        show_help
        exit 0
    fi

    local cmd="${1:-}"
    shift || true

    case "$cmd" in
        sync)     cmd_sync "$@" ;;
        build)    cmd_build "$@" ;;
        template) cmd_template "$@" ;;
        release)  cmd_release "$@" ;;
        status)   cmd_status "$@" ;;
        help|--help|-h) show_help ;;
        *) log_err "Unknown command: $cmd"; show_help; exit 1 ;;
    esac
}

main "$@"
