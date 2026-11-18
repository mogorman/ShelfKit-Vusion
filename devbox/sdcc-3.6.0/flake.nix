{
  description = "SDCC 3.6.0 (mcs51) for ShelfKit firmware";

  # gcc-12 stdenv: compiles the 2021 SDCC source cleanly; newer stdenvs may not.
  inputs.nixpkgs.url = "github:nixos/nixpkgs/22.05";

  outputs = { self, nixpkgs }:
    let
      forSystem = system:
        let pkgs = nixpkgs.legacyPackages.${system}; in
        {
          sdcc360 = pkgs.stdenv.mkDerivation {
            pname = "sdcc";
            version = "3.6.0";
            src = pkgs.fetchurl {
              url = "https://sourceforge.net/projects/sdcc/files/sdcc/3.6.0/sdcc-src-3.6.0.tar.bz2/download";
              name = "sdcc-src-3.6.0.tar.bz2";
              hash = "sha256-6F3OsR4B/++1Rew4nakSZRMMkZU1iTkt3dLl7At8o3Q=";
            };
            nativeBuildInputs = [ pkgs.autoconf pkgs.automake pkgs.bison pkgs.flex ];
            buildInputs = [ pkgs.boost pkgs.zlib pkgs.texinfo ];
            # the 3.6.0 tarball ships no generated configure; autogen if needed
            preConfigure = ''
              [ -x configure ] || ./autogen.sh
            '';
            # build only mcs51 -> sdcc + sdas8051 + packihx; skip the other 13 ports
            configureFlags = [
              "--disable-ds390-port" "--disable-ds400-port" "--disable-gbz80-port"
              "--disable-hc08-port" "--disable-pic14-port" "--disable-pic16-port"
              "--disable-r2k-port" "--disable-r3ka-port" "--disable-s08-port"
              "--disable-stm8-port" "--disable-tlcs90-port" "--disable-z80-port"
              "--disable-z180-port"
            ];
            meta.description = "Small Device C Compiler (mcs51 only)";

            # The nix SDCC links against its own glibc 2.34 closure (via RUNPATH). A devbox
            # profile puts a newer libstdc++ on LD_LIBRARY_PATH, which wins over RUNPATH and
            # crashes the binary with a GLIBC version error. Two things must hold so
            # `sdcc`/`sdas8051`/`packihx` just work on PATH with no manual env:
            #   1) each top-level tool is a wrapper that clears LD_LIBRARY_PATH first, so the
            #      loader falls back to RUNPATH (the right closure);
            #   2) the driver finds its sibling tools (sdcpp/sdas8051/sdld) by its own argv[0]
            #      directory, so the real tools keep their names under bin/.sdcc/ and the
            #      wrappers exec them from there - renaming sdcc to .sdcc-real would make it
            #      look for .sdcpp-real and fail. share/ (the include path) is a compiled-in
            #      absolute prefix and is left untouched.
            postFixup = ''
              mkdir -p "$out/bin/.sdcc"
              for t in sdcc sdcpp sdas8051 sdld sdar sdranlib sdcdb sdobjcopy sdnm s51 sdcclib as2gbmap makebin packihx; do
                [ -e "$out/bin/$t" ] && mv "$out/bin/$t" "$out/bin/.sdcc/$t"
              done
              mkwrapper() {
                tool="''$1"
                [ -x "$out/bin/.sdcc/$tool" ] || return 0
                real="$out/bin/.sdcc/''${tool}"
                {
                  echo '#!/bin/sh'
                  echo 'unset LD_LIBRARY_PATH'
                  echo "exec \"$real\" \"\$@\""
                } > "$out/bin/$tool"
                chmod +x "$out/bin/$tool"
              }
              for t in sdcc sdas8051 packihx; do mkwrapper "$t"; done
            '';
          };
        };
    in {
      packages.x86_64-linux = forSystem "x86_64-linux";
      packages.aarch64-linux = forSystem "aarch64-linux";
    };
}
