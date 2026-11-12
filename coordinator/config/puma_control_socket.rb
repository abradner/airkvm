# Where Puma's control socket lives, and the checks that make that place safe.
#
# Loaded by config/puma.rb with require_relative, and by its spec. It lives in
# config/ rather than lib/ on purpose: config.autoload_lib hands lib/ to
# Zeitwerk, and a file that Puma also requires directly would be loaded twice.
#
# Why this needs code at all. The socket cannot live in `<app>/tmp/`: a unix
# socket path is capped at 108 bytes, and every agent worktree in this repo puts
# the app under `.claude/worktrees/<name>/coordinator/`, where that path is 110 —
# Puma then refuses to start with "too long unix socket path". So it goes in the
# system temp dir, which is shared, which is why the rest of this file exists:
#
#   * Puma creates unix sockets with umask 0, i.e. srwxrwxrwx, so the socket's
#     own mode protects nothing. Protection has to come from its directory.
#   * Puma's `?umask=` query parameter would fix the mode, but yabeda's stats
#     fetcher opens the control URL as a literal path (`gsub("unix://", "")`),
#     so it looks for a file literally named `pumactl.sock?umask=0077` and every
#     /metrics request fails with ENOENT. Measured, not guessed.
#   * The directory name is a hash of the app root, which is predictable. A
#     local user could create it first — or plant a symlink there — to capture
#     or intercept the socket. So an existing directory is accepted only if it is
#     a real directory (lstat, never following a link), owned by us, mode 0700.
#     Anything else refuses to boot rather than quietly using it.
#   * Those checks hold only if nobody else can rename or replace entries in
#     the parent: in a group- or world-writable parent without the sticky bit,
#     another user could swap the verified directory for a symlink between the
#     check and Puma's bind. The system /tmp is sticky; a TMPDIR pointed at a
#     shared writable directory is refused.
require "digest"
require "tmpdir"

module PumaControlSocket
  class Error < StandardError; end
  class UnsafeDirectory < Error; end
  class PathTooLong < Error; end

  SOCKET_NAME = "pumactl.sock"
  # sun_path is 108 bytes including the terminating NUL on Linux, but 104 on
  # macOS and the BSDs — and the operator's machine is a Mac. The smaller limit
  # applies everywhere, so a path that works here cannot fail there.
  MAX_SOCKET_PATH = 103

  module_function

  def directory_for(app_root, tmpdir: Dir.tmpdir)
    File.join(tmpdir, "coordinator-#{Digest::SHA256.hexdigest(File.expand_path(app_root))[0, 12]}")
  end

  # Returns the socket path, creating and verifying its directory.
  def prepare!(app_root, tmpdir: Dir.tmpdir)
    dir = directory_for(app_root, tmpdir: tmpdir)
    path = File.join(dir, SOCKET_NAME)
    if path.bytesize > MAX_SOCKET_PATH
      raise PathTooLong, "Puma control socket path is #{path.bytesize} bytes; " \
                         "unix sockets allow #{MAX_SOCKET_PATH} here. Point TMPDIR somewhere shorter."
    end

    verify_parent!(tmpdir)
    begin
      Dir.mkdir(dir, 0o700)
      # mkdir's mode is filtered through the umask; a restrictive one (0277)
      # would leave 0500 and fail the check below for a directory we just made.
      File.chmod(0o700, dir)
    rescue Errno::EEXIST
      # Already there — from a previous boot, or someone else's. verify! decides.
    end
    verify!(dir)
    path
  end

  def verify_parent!(parent)
    stat = File.stat(parent)
    shared_writable = (stat.mode & 0o022) != 0
    return unless shared_writable && !stat.sticky?

    raise UnsafeDirectory, format(
      "Refusing to put Puma's control socket under %s: it is writable by other users " \
      "(mode %04o) without the sticky bit, so they could replace the socket's directory " \
      "after it is checked. Point TMPDIR at a directory only you can write.",
      parent, stat.mode & 0o7777
    )
  end

  def verify!(dir)
    stat = File.lstat(dir)
    problem =
      if stat.symlink? then "is a symlink"
      elsif !stat.directory? then "is not a directory"
      elsif !stat.owned? then "is owned by another user"
      elsif (stat.mode & 0o777) != 0o700 then format("has mode %04o, not 0700", stat.mode & 0o777)
      end
    return unless problem

    raise UnsafeDirectory, <<~MESSAGE
      Refusing to put Puma's control socket in #{dir}: it #{problem}.

      The socket gives whoever can reach it control of this server, and the
      directory is the only thing restricting who can. Remove it (or fix it) and
      start again; it is recreated with mode 0700 on boot.
    MESSAGE
  end
end
