# mrbgems

mrbgems is a library manager to integrate C and Ruby extension in an easy and
standardised way into mruby.

## Usage

By default mrbgems is currently deactivated. As long as mrbgems is deactivated
there is no overhead inside of the mruby interpreter.

To activate you have to make the following changes:
* set ```ENABLE_GEMS``` to ```true``` in *$(MRUBY_ROOT)/Makefile*
* activate GEMs in *$(MRUBY_ROOT)/mrbgems/GEMS.active*

Notice that we do not need to comment out ```DISABLE_GEMS```
in *$(MRUBY_ROOT)/include/mrbconf.h*, since this flag will now be included as
a command line flag in *$(MRUBY_ROOT)/Rakefile*.

Every activated GEM has to be listed in *GEMS.active*. You have to point to
the GEM directory absolute or relative (based on *mrbgems/g*). It is possible
to point to an alternative activate file:
* set ```ACTIVE_GEMS``` to your customized GEM list in *$(MRUBY_ROOT)/Makefile*

## GEM Structure

The maximal GEM structure looks like this:

```
+- GEM_NAME         <- Name of GEM
   |
   +- include/      <- Header files for C extension
   |
   +- mrblib/       <- Source for Ruby extension
   |
   +- src/          <- Source for C extension
   |
   +- test/         <- Test code (Ruby)
   |
   +- Makefile      <- Makefile for GEM
   |
   +- README.md     <- Readme for GEM
```

The folder *mrblib* contains pure Ruby files to extend mruby. The folder *src*
contains C files to extend mruby. The folder *test* contains pure Ruby files
for testing purposes which will be used by ```mrbtest```. The *Makefile* contains
rules to build a *mrb-GEMNAME-gem.a* file inside of the GEM directory. Which
will be used for integration into the normal mruby build process. *README.md*
is a short description of your GEM.

## Build process

mrbgems will call ```make``` to build and *make clean* to clean your GEM. You
have to build a *mrb-GEMNAME-gem.a* file during this build process. How you
are going to do this is up to you.

To make your build process more easier and more standardized we suggest
to include *mrbgems/Makefile4gem* which defines some helper rules. In
case you include this Makefile you have to define specific pre-defined
rules like ```gem-all``` for the build process and ```gem-clean``` for
the clean process. There are additional helper rules for specific GEM
examples below.

## C Extension

mruby can be extended with C. It is possible by using the C API to
integrate C libraries into mruby.

The *Makefile* is used for building a C extension. You should
define ```GEM``` (GEM name), ```GEM_C_FILES``` (all C files) and
```GEM_OBJECTS``` (all Object files). Pay also attention that your
*Makefile* has to build the object files. You can use
```gem-c-files``` to build a *mrb-GEMNAME-gem.a* out of your
Object code and use ```gem-clean-c-files``` to clean the object files.

### Pre-Conditions

mrbgems expects that you have implemented a C method called
```mrb_YOURGEMNAME_gem_init(mrb_state)```. ```YOURGEMNAME``` will be replaced
by the name of you GEM. The directory name of your GEM is considered also
as the name! If you call your GEM directory *c_extension_example*, your
initialisation method could look like this:

```
void
mrb_c_extension_example_gem_init(mrb_state* mrb) {
  _class_cextension = mrb_define_module(mrb, "CExtension");
  mrb_define_class_method(mrb, _class_cextension, "c_method", mrb_c_method, ARGS_NONE());
}
```

mrbgems will also use the *gem-clean* make target to clean up your GEM. Implement
this target with the necessary rules!

### Example

```
+- c_extension_example/
   |
   +- src/
   |  |
   |  +- example.c         <- C extension source
   |
   +- test/
   |  |
   |  +- example.rb        <- Test code for C extension
   |
   +- Makefile             <- Build rules for C extension
   |
   +- README.md
```

## Ruby Extension

mruby can be extended with pure Ruby. It is possible to override existing
classes or add new ones in this way. Put all Ruby files into the *mrblib*
folder.

The *Makefile* is used for building a Ruby extension. You should  define
```GEM``` (GEM name) and *GEM_RB_FILES* (all Ruby files). You can use
```gem-rb-files``` to build a *mrb-GEMNAME-gem.a* out of your Ruby code and use
```gem-clean-rb-files``` to clean the generated C files.

### Pre-Conditions

mrbgems will automatically call the ```gem-all``` make target of your GEM.

mrbgems will also use the ```gem-clean``` make target to clean up your GEM. Implement
this target with the necessary rules!

### Example

```
+- ruby_extension_example/
   |
   +- mrblib/
   |  |
   |  +- example.rb        <- Ruby extension source
   |
   +- test/
   |  |
   |  +- example.rb        <- Test code for Ruby extension
   |
   +- Makefile
   |
   +- README.md
```

## C and Ruby Extension

mruby can be extended with C and Ruby at the same time. It is possible to
override existing classes or add new ones in this way. Put all Ruby files
into the *mrblib* folder and all C files into the *src* folder.

The *Makefile* is used for building a C and Ruby extension. You should
define ```GEM``` (GEM name), ```GEM_C_FILES``` (all C files),
```GEM_OBJECTS``` (all Object files) and ```GEM_RB_FILES``` (all Ruby
files). You can use ```gem-c-and-rb-files``` to build a
*mrb-GEMNAME-gem.a* out of your Object and Ruby code. Use
```gem-clean-c-and-rb-files``` to clean the generated C files. 

### Pre-Conditions

See C and Ruby example.

### Example

```
+- c_and_ruby_extension_example/
   |
   +- mrblib/
   |  |
   |  +- example.rb        <- Ruby extension source
   |
   +- src/
   |  |
   |  +- example.c         <- C extension source
   |
   +- test/
   |  |
   |  +- example.rb        <- Test code for C and Ruby extension
   |
   +- Makefile
   |
   +- README.md
