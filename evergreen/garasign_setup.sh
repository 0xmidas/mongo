DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

set -o errexit
set -o verbose

echo "${release_tools_container_registry_password}" | podman login --password-stdin --username ${release_tools_container_registry_username} ${release_tools_container_registry}
