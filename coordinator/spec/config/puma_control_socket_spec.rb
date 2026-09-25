require "rails_helper"
require "tmpdir"
require Rails.root.join("config/puma_control_socket")

# Every branch of the directory check is a way the control socket could end up
# reachable by someone it should not be. Each is exercised against a private
# scratch directory standing in for the system temp dir, so no example touches
# the real one.
#
# Not covered here, asserted by reading instead and recorded rather than
# silently skipped: a directory owned by ANOTHER user (creating one needs root),
# and losing the mkdir race to another process (rescued EEXIST, after which the
# same checks decide).
RSpec.describe PumaControlSocket do
  around do |example|
    Dir.mktmpdir("pcs-spec") do |tmp|
      @tmp = tmp
      example.run
    end
  end

  let(:app_root) { "/srv/some/app" }
  let(:dir) { described_class.directory_for(app_root, tmpdir: @tmp) }

  it "creates the directory owner-only and returns a socket inside it" do
    path = described_class.prepare!(app_root, tmpdir: @tmp)

    expect(path).to eq(File.join(dir, "pumactl.sock"))
    expect(File.stat(dir).mode & 0o777).to eq(0o700)
    expect(File.stat(dir)).to be_owned
  end

  it "reuses a directory it already created" do
    described_class.prepare!(app_root, tmpdir: @tmp)
    expect { described_class.prepare!(app_root, tmpdir: @tmp) }.not_to raise_error
  end

  it "refuses a pre-existing directory that other users can enter" do
    Dir.mkdir(dir, 0o700)
    File.chmod(0o755, dir)

    expect { described_class.prepare!(app_root, tmpdir: @tmp) }
      .to raise_error(PumaControlSocket::UnsafeDirectory, /mode 0755/)
  end

  it "refuses a symlink planted where the directory should be" do
    # The name is a predictable hash, so another local user can create it first.
    # A symlink there would redirect the socket somewhere the attacker controls;
    # lstat is what stops the check following it.
    target = File.join(@tmp, "attacker")
    Dir.mkdir(target, 0o700)
    File.symlink(target, dir)

    expect { described_class.prepare!(app_root, tmpdir: @tmp) }
      .to raise_error(PumaControlSocket::UnsafeDirectory, /symlink/)
  end

  it "refuses a plain file squatting on the name" do
    File.write(dir, "")

    expect { described_class.prepare!(app_root, tmpdir: @tmp) }
      .to raise_error(PumaControlSocket::UnsafeDirectory, /not a directory/)
  end

  it "does not let the app's depth reach the socket path" do
    # The whole reason the socket left <app>/tmp/: this repo's worktrees make
    # that path 110 bytes. Here the app root only feeds a fixed-length hash.
    shallow = described_class.prepare!("/a", tmpdir: @tmp)
    deep = described_class.prepare!("/home/someone/" + ("x" * 200) + "/coordinator", tmpdir: @tmp)

    expect(deep.bytesize).to eq(shallow.bytesize)
  end

  it "refuses a TMPDIR long enough to break the unix socket limit" do
    # 103 bytes, not Linux's 107: macOS and the BSDs cap sun_path at 104.
    long = File.join(@tmp, "t" * (PumaControlSocket::MAX_SOCKET_PATH - @tmp.bytesize))
    Dir.mkdir(long, 0o700)

    expect { described_class.prepare!(app_root, tmpdir: long) }
      .to raise_error(PumaControlSocket::PathTooLong, /allow 103/)
  end

  it "refuses a parent other users can write to, unless it is sticky" do
    File.chmod(0o777, @tmp)
    expect { described_class.prepare!(app_root, tmpdir: @tmp) }
      .to raise_error(PumaControlSocket::UnsafeDirectory, /without the sticky bit/)

    File.chmod(0o1777, @tmp) # like /tmp: others may write, but not rename our entries
    expect { described_class.prepare!(app_root, tmpdir: @tmp) }.not_to raise_error
  ensure
    File.chmod(0o700, @tmp)
  end

  it "creates the directory 0700 even under a restrictive umask" do
    old = File.umask(0o277)
    described_class.prepare!(app_root, tmpdir: @tmp)
    expect(File.stat(dir).mode & 0o777).to eq(0o700)
  ensure
    File.umask(old)
  end

  it "gives different checkouts different directories" do
    a = described_class.directory_for("/one/coordinator", tmpdir: @tmp)
    b = described_class.directory_for("/two/coordinator", tmpdir: @tmp)
    expect(a).not_to eq(b)
  end
end
