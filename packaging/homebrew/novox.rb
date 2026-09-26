# Homebrew formula template. Fill in url/sha256 from a GitHub release source tarball, then put it
# in a tap repo (github.com/<you>/homebrew-tap) so people can `brew install <you>/tap/novox`.
class Novox < Formula
  desc "Formant singing synthesizer: type lyrics, give it a melody, it sings"
  homepage "https://github.com/OWNER/REPO"
  url "https://github.com/OWNER/REPO/archive/refs/tags/v0.1.0.tar.gz"
  sha256 "FILL_ME_IN"
  license "MIT"

  depends_on "cmake" => :build
  depends_on "python@3.12" => :build  # embeds the pronouncing dictionary at build time

  def install
    system "cmake", "-S", ".", "-B", "build", "-DNOVOX_BUILD_TESTS=OFF", *std_cmake_args
    system "cmake", "--build", "build", "--target", "novox"
    bin.install "build/novox"
  end

  test do
    system bin/"novox", "say", "hello", "-o", testpath/"hello.wav"
    assert_predicate testpath/"hello.wav", :exist?
  end
end
