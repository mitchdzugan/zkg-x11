{ pkgs ? import <nixpkgs> {} }:
  pkgs.mkShell {
    # nativeBuildInputs is usually what you want -- tools you need to run
    nativeBuildInputs = with pkgs.buildPackages; [ xorg.libxcb xorg.xcbutil xorg.xcbutilkeysyms gnumake ];
}
