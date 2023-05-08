#!/bin/bash -ex

usage() {
    echo "Usage: $0 <artifact_type> <version> <url>"
}

if [ $# -lt 3 ]; then
  usage
  exit 1
fi
artifact_type="$1"
version="$2"
url="$3"

readme_path="README.md"

latest_release_comment="<!--${artifact_type}-latest-release-->"

pretty_artifact_name() {
    case "${artifact_type}" in
        cli) echo "CLI";;
        vizier) echo "Vizier";;
        operator) echo "Operator";;
        cloud) echo "Cloud";;
    esac
}

latest_release_line() {
  echo "- ${latest_release_comment}[$(pretty_artifact_name) ${version}](${url})"
}

sed -i 's|.*'"${latest_release_comment}"'.*|'"$(latest_release_line)"'|' "${readme_path}"

echo "[bot][releases] Update readme with link to latest ${artifact_type} release." > pr_title
cat <<EOF > pr_body
Summary: TSIA

Type of change: /kind cleanup

Test Plan: N/A
EOF
