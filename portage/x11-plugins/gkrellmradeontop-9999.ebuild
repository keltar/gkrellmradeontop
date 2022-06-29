# Copyright 2022 Gentoo Authors
# Distributed under the terms of the GNU General Public License v2

EAPI=8
inherit gkrellm-plugin toolchain-funcs

DESCRIPTION=""
HOMEPAGE=""
SRC_URI=""

if [[ ${PV} == "9999" ]]; then
	EGIT_REPO_URI="https://github.com/keltar/gkrellmradeontop"
	EGIT_REPO_BRANCH="main"
	inherit git-r3
fi

LICENSE="GPL-3"
SLOT="0"
KEYWORDS="~amd64 ~x86"

DEPEND="app-misc/radeontop[video_cards_amdgpu]
	app-admin/gkrellm:2[X]
	dev-libs/glib:2
	x11-libs/gtk+:2
"
RDEPEND="${DEPEND}"
BDEPEND="virtual/pkgconfig"

src_configure() {
	tc-export CC PKG_CONFIG
	PLUGIN_SO=(gkrellmradeontop$(get_modname))
	default
}

src_compile() {
	emake CFLAGS="${CFLAGS} -fPIC `pkg-config gtk+-2.0 --cflags`"
}
