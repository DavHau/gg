{
  description = "The Stanford Builder";

  inputs = {
    nixpkgs.url = "nixpkgs/nixos-unstable";
  };

  outputs = { self, nixpkgs, }@inp:
  let
    l = nixpkgs.lib // builtins;

    supportedSystems = [ "x86_64-linux" ];

    forAllSystems = f: l.genAttrs supportedSystems
      (system: f system (import nixpkgs { inherit system; }));

  in
  {
    defaultPackage =
      forAllSystems (system: pkgs: self.packages."${system}".gg);

    packages = forAllSystems (system: pkgs: {
      gg = pkgs.stdenv.mkDerivation {
        pname = "gg";
        version = "unknown";
        src = self;

        TOOLCHAIN_PATH = "${pkgs.gcc.cc}/bin";

        patches = [
          ./configure.ac.patch
        ];

        postPatch = ''
          patchShebangs ./src

          toReplace=$(find -name '*.cc' -or -name '*.hh' -or -name '*.ac')

          for f in $toReplace; do
            if cat $f | grep -q "crypto++"; then
              substituteInPlace $f --replace \
                "crypto++" \
                "cryptopp"
            fi
          done

          substituteInPlace src/models/generate-toolchain-header.py --replace \
            "gcc-7" \
            "gcc"
        '';

        # TODO: glibc seems to be required, but including it introduces errors
        #       when building protobuf.
        nativeBuildInputs = with pkgs; [
          autoreconfHook
          boost
          hiredis
          libcap
          openssl
          cryptopp
          pkg-config
          protobuf
          python3
          breakpointHook
        ];

        b = "${pkgs.busybox}/bin/busybox";

        buildInputs = with pkgs; [
        ];
      };
    });
  };
}
