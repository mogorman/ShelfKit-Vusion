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
          };
        };
    in {
      packages.x86_64-linux = forSystem "x86_64-linux";
      packages.aarch64-linux = forSystem "aarch64-linux";
    };
}
