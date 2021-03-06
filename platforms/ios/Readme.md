# Setting up build environment for iOS #

## Prerequisites ##
 - Installed XCode  

## Buildroot ##
A 'buildroot' is a prefix (i.e. a directory), containing a `lib` and `include` subdirs,
where all built libraries and headers will be installed, and
third-party dependencies (i.e. non-system libs and headers) will be looked
for. For each target - device and simulator you need one such tree.
To create a buildroot, just create the root dir, and the `lib` and `include` subdirs.

## Environment script ##
After you have the necessary buildroot, verify that the user-settable variables
in the `ios-env.sh` script are correctly set, and **source** it into your shell.
It should print instructions how to user it with autotools and cmake.
Libs built using this environment will be installed in the buildroot specified
in the script.
You can switch targets and buildroot by updating the input variables in the
script and then source it again in the same or a new shell.
