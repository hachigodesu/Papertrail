# Run in QtKeychain's source folder as it's fetched. It assumes that Windows means Microsoft's compiler, whose /utf-8
# flag GCC takes to be a file name. GCC already reads source as UTF-8, so the flag is simply taken out.
file(READ qtkeychain/CMakeLists.txt contents)
string(REPLACE "add_definitions( /utf-8 -DUNICODE )" "add_definitions( -DUNICODE )" contents "${contents}")
file(WRITE qtkeychain/CMakeLists.txt "${contents}")

# Its own test needs a credential store to run against, and isn't ours to run.
file(READ CMakeLists.txt contents)
string(REPLACE "add_subdirectory(autotest)" "" contents "${contents}")
file(WRITE CMakeLists.txt "${contents}")
