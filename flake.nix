{
  description = "better chibicc";

  inputs = {
    # nixpkgs unstable for latest versions
    pkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils, ... } @ inputs: 
    (flake-utils.lib.eachDefaultSystem (system:
      let
        repo_root = builtins.toString ./.;

        pkgs = import nixpkgs { inherit system; };

        deps = with pkgs; [
          gcc
          gdb
          glibc
          glibc.static
          stdenv
          curl.dev
          pkg-config
        ];
      in {
        devShells.default = pkgs.mkShell {
          packages = deps;
          nativeBuildInputs = deps;
          buildInputs = deps;
          env = let
            concat = paths: (builtins.concatStringsSep ":" paths) + ":";
            cc = pkgs.stdenv.cc;
          in {
            LIBRARY_PATH = concat [
              "${pkgs.glibc.out}/lib"
              "${pkgs.gcc.out}/lib"
              "${pkgs.curl.out}/lib"
            ];
            LD_LIBRARY_PATH = concat [
              "${cc.cc}/lib/gcc/${pkgs.stdenv.hostPlatform.config}/${cc.version}"
            ];
            CFLAGS = "-I${pkgs.curl.dev}/include";
            LDFLAGS = "-L${pkgs.curl.out}/lib";
          };
          shellHook = ''
            export NIX_LDFLAGS="-L$(dirname $(g++ -print-file-name=libgcc_s.so)) $NIX_LDFLAGS"
          '';
        };
      })
    );
}
