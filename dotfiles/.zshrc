export ZSH="$HOME/.oh-my-zsh"

#use ninja for cmake builds
export EZA_CONFIG_DIR="$HOME/.config/eza"

plugins=(git
 # Version Control
  git

  # Cloud & Infrastructure
  docker
  docker-compose
  kubectl
  helm
  terraform
  aws
  gcloud
  azure

  # Programming Languages & Tools
  python
  pip
  node
  npm
  yarn
  golang
  rust

  # Productivity Boosters
  sudo
  extract           # Universal archive extractor (works with .tar, .zip, .gz, etc.)
  z                 # Jump to frequently used directories
  history           # Enhanced history commands
  command-not-found # Suggests package to install for missing commands
  vscode            # VS Code aliases and shortcuts

  # Community Plugins (must be last)
  zsh-autosuggestions
  zsh-syntax-highlighting
)


# theme
ZSH_THEME="hoshinoht"


export EDITOR=nvim

source $ZSH/oh-my-zsh.sh

# For pico development
PICO_SDK_PATH=~/Projects/sit/inf2004/pico-sdk
PICO_BOARD=pico_w

### Added by Zinit's installer
if [[ ! -f $HOME/.local/share/zinit/zinit.git/zinit.zsh ]]; then
    print -P "%F{33} %F{220}Installing %F{33}ZDHARMA-CONTINUUM%F{220} Initiative Plugin Manager (%F{33}zdharma-continuum/zinit%F{220})…%f"
    command mkdir -p "$HOME/.local/share/zinit" && command chmod g-rwX "$HOME/.local/share/zinit"
    command git clone https://github.com/zdharma-continuum/zinit "$HOME/.local/share/zinit/zinit.git" && \
        print -P "%F{33} %F{34}Installation successful.%f%b" || \
        print -P "%F{160} The clone has failed.%f%b"
fi

# setup eza (ls)
alias ls='eza --icons --group-directories-first' 
alias ll='eza -la --icons --group-directories-first'
alias l='eza -l --icons --group-directories-first'

# change nvim to vim
alias vim='nvim'
# python3 to python
alias python='python3'
alias pip='pip3'


unsetopt prompt_sp

export PATH="$PATH":"$HOME/.pub-cache/bin"
# Added by Antigravity
export PATH="/Users/cantabile/.antigravity/antigravity/bin:$PATH"
export PATH="$HOME/.local/bin:$PATH"
export PATH="$PATH:$HOME/.pub-cache/bin"

export IDF_PATH=~/esp/esp-idf
export PATH="$IDF_PATH/tools:$PATH"
