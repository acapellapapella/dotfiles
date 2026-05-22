export ZSH="$HOME/.oh-my-zsh"
#shellcheck disable=SC2034
ZSH_THEME="gentoo"

#shellcheck disable=SC2034
plugins=(
    git
    sudo
    zsh-autosuggestions
    zsh-syntax-highlighting
)

source $ZSH/oh-my-zsh.sh

typeset -A ZSH_HIGHLIGHT_STYLES

ZSH_HIGHLIGHT_STYLES[command]='fg=green,bold'
ZSH_HIGHLIGHT_STYLES[unknown-token]='fg=red,bold'
ZSH_HIGHLIGHT_STYLES[alias]='fg=green,bold'
ZSH_HIGHLIGHT_STYLES[builtin]='fg=green'
ZSH_HIGHLIGHT_STYLES[path]='fg=cyan,underline'
ZSH_HIGHLIGHT_STYLES[single-hyphen-option]='fg=yellow'
ZSH_HIGHLIGHT_STYLES[double-hyphen-option]='fg=yellow'
#shellcheck disable=SC2034
ZSH_HIGHLIGHT_STYLES[argument]='fg=white'

bindkey '^H' backward-kill-word

export PATH="$HOME/.local/bin:$PATH"
