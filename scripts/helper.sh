# Ensures passed in version values are supported.
function check-version-numbers() {
  CHECK_VERSION_MAJOR=$1
  CHECK_VERSION_MINOR=$2

  if [[ $CHECK_VERSION_MAJOR -lt $FLON_MIN_VERSION_MAJOR ]]; then
    exit 1
  fi
  if [[ $CHECK_VERSION_MAJOR -gt $FLON_MAX_VERSION_MAJOR ]]; then
    exit 1
  fi
  if [[ $CHECK_VERSION_MAJOR -eq $FLON_MIN_VERSION_MAJOR ]]; then
    if [[ $CHECK_VERSION_MINOR -lt $FLON_MIN_VERSION_MINOR ]]; then
      exit 1
    fi
  fi
  if [[ $CHECK_VERSION_MAJOR -eq $FLON_MAX_VERSION_MAJOR ]]; then
    if [[ $CHECK_VERSION_MINOR -gt $FLON_MAX_VERSION_MINOR ]]; then
      exit 1
    fi
  fi
  exit 0
}


# Handles choosing which FLON directory to select when the default location is used.
function default-flon-directories() {
  REGEX='^[0-9]+([.][0-9]+)?$'
  ALL_FLON_SUBDIRS=()
  if [[ -d ${HOME}/flon ]]; then
    ALL_FLON_SUBDIRS=($(ls ${HOME}/flon | sort -V))
  fi
  for ITEM in "${ALL_FLON_SUBDIRS[@]}"; do
    if [[ "$ITEM" =~ $REGEX ]]; then
      DIR_MAJOR=$(echo $ITEM | cut -f1 -d '.')
      DIR_MINOR=$(echo $ITEM | cut -f2 -d '.')
      if $(check-version-numbers $DIR_MAJOR $DIR_MINOR); then
        PROMPT_FLON_DIRS+=($ITEM)
      fi
    fi
  done
  for ITEM in "${PROMPT_FLON_DIRS[@]}"; do
    if [[ "$ITEM" =~ $REGEX ]]; then
      FLON_VERSION=$ITEM
    fi
  done
}


# Prompts or sets default behavior for choosing FLON directory.
function flon-directory-prompt() {
  if [[ -z $FLON_DIR_PROMPT ]]; then
    default-flon-directories;
    echo 'No FLON location was specified.'
    while true; do
      if [[ $NONINTERACTIVE != true ]]; then
        if [[ -z $FLON_VERSION ]]; then
          echo "No default FLON installations detected..."
          PROCEED=n
        else
          printf "Is FLON installed in the default location: $HOME/flon/$FLON_VERSION (y/n)" && read -p " " PROCEED
        fi
      fi
      echo ""
      case $PROCEED in
        "" )
          echo "Is FLON installed in the default location?";;
        0 | true | [Yy]* )
          break;;
        1 | false | [Nn]* )
          if [[ $PROMPT_FLON_DIRS ]]; then
            echo "Found these compatible FLON versions in the default location."
            printf "$HOME/flon/%s\n" "${PROMPT_FLON_DIRS[@]}"
          fi
          printf "Enter the installation location of FLON:" && read -e -p " " FLON_DIR_PROMPT;
          FLON_DIR_PROMPT="${FLON_DIR_PROMPT/#\~/$HOME}"
          break;;
        * )
          echo "Please type 'y' for yes or 'n' for no.";;
      esac
    done
  fi
  export FLON_INSTALL_DIR="${FLON_DIR_PROMPT:-${HOME}/flon/${FLON_VERSION}}"
}


# Prompts or default behavior for choosing FLON.CDT directory.
function cdt-directory-prompt() {
  if [[ -z $CDT_DIR_PROMPT ]]; then
    echo 'No FLON.CDT location was specified.'
    while true; do
      if [[ $NONINTERACTIVE != true ]]; then
        printf "Is FLON.CDT installed in the default location? /usr/local/flon.cdt (y/n)" && read -p " " PROCEED
      fi
      echo ""
      case $PROCEED in
        "" )
          echo "Is FLON.CDT installed in the default location?";;
        0 | true | [Yy]* )
          break;;
        1 | false | [Nn]* )
          printf "Enter the installation location of FLON.CDT:" && read -e -p " " CDT_DIR_PROMPT;
          CDT_DIR_PROMPT="${CDT_DIR_PROMPT/#\~/$HOME}"
          break;;
        * )
          echo "Please type 'y' for yes or 'n' for no.";;
      esac
    done
  fi
  export CDT_INSTALL_DIR="${CDT_DIR_PROMPT:-/usr/local/flon.cdt}"
}


# Ensures FLON is installed and compatible via version listed in tests/CMakeLists.txt.
function amnod-version-check() {
  INSTALLED_VERSION=$(echo $($FLON_INSTALL_DIR/bin/amnod --version))
  INSTALLED_VERSION_MAJOR=$(echo $INSTALLED_VERSION | cut -f1 -d '.' | sed 's/v//g')
  INSTALLED_VERSION_MINOR=$(echo $INSTALLED_VERSION | cut -f2 -d '.' | sed 's/v//g')

  if [[ -z $INSTALLED_VERSION_MAJOR || -z $INSTALLED_VERSION_MINOR ]]; then
    echo "Could not determine FLON version. Exiting..."
    exit 1;
  fi

  if $(check-version-numbers $INSTALLED_VERSION_MAJOR $INSTALLED_VERSION_MINOR); then
    if [[ $INSTALLED_VERSION_MAJOR -gt $FLON_SOFT_MAX_MAJOR ]]; then
      echo "Detected FLON version is greater than recommended soft max: $FLON_SOFT_MAX_MAJOR.$FLON_SOFT_MAX_MINOR. Proceed with caution."
    fi
    if [[ $INSTALLED_VERSION_MAJOR -eq $FLON_SOFT_MAX_MAJOR && $INSTALLED_VERSION_MINOR -gt $FLON_SOFT_MAX_MINOR ]]; then
      echo "Detected FLON version is greater than recommended soft max: $FLON_SOFT_MAX_MAJOR.$FLON_SOFT_MAX_MINOR. Proceed with caution."
    fi
  else
    echo "Supported versions are: $FLON_MIN_VERSION_MAJOR.$FLON_MIN_VERSION_MINOR - $FLON_MAX_VERSION_MAJOR.$FLON_MAX_VERSION_MINOR"
    echo "Invalid FLON installation. Exiting..."
    exit 1;
  fi
}
