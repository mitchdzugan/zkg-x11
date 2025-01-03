{
  description = "(x11) keyboard grabber and key press reporter";
  inputs = {
    nixpkgs.url = "nixpkgs";
    flake-utils.url = "github:numtide/flake-utils";
  };
  outputs = { self, nixpkgs, flake-utils, ... }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = nixpkgs.legacyPackages.${system};
      in rec {
        packages.default = pkgs.stdenv.mkDerivation {
          pname = "zkg-x11";
          version = "0.1";
          src = ./.;
          nativeBuildInputs = with pkgs; [
            xorg.libxcb
            xorg.xcbutil
            xorg.xcbutilkeysyms
            gnumake
          ];
          propagatedBuildInputs = with pkgs; [
            xorg.libxcb
            xorg.xcbutil
            xorg.xcbutilkeysyms
          ];
          buildPhase = "make";
          installPhase = ''
            mkdir -p "$out/bin"
            mv zkg "$out/bin/"
          '';
        };
      });
}
