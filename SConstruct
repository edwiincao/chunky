import os
import sys

# Define build variables.
vars = Variables()
vars.Add(EnumVariable('target', 'Set build type', 'debug', allowed_values=('debug', 'release')))

env = Environment(ENV=os.environ, variables=vars)
env.Decider('MD5-timestamp')

# Boilerplate for help (scons -h).
env.Help(vars.GenerateHelpText(env))
unknownVariables = vars.UnknownVariables()
if unknownVariables:
    print 'Unknown variables:', unknownVariables.keys()
    Exit(1)

# Target configuration.
if env['target'] == 'debug':
    env.Append(CXXFLAGS=['-g'])

elif env['target'] == 'release':
    env.Append(CXXFLAGS=['-O2'])
    env.Append(CPPDEFINES=['NDEBUG', 'BOOST_DISABLE_ASSERTS'])

# Platform configuration.
if sys.platform.startswith('darwin'):
    env.Replace(CXX = 'clang++')
    env.Append(CXXFLAGS = '-std=c++11')

elif sys.platform.startswith('linux'):
    env.Append(CXXFLAGS = '-std=c++0x')

env.Append(LIBS=['boost_system-mt'])

# Project configuration.
env.Append(CXXFLAGS=['-march=native'])
env.Append(CXXFLAGS=['-Wall', '-Werror'])

TEST_SOURCES = os.popen('ls *.cpp').read().split()
for test_source in TEST_SOURCES:
    test_name = os.path.splitext(test_source)[0]
    test = env.Program(test_name,[test_source])
    env.Default(test)
