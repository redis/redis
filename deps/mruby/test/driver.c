/*
** mrbtest - Test for Embeddable Ruby
**
** This program runs Ruby test programs in test/t directory
** against the current mruby implementation.
*/

#include <string.h>

#include <mruby.h>
#include <mruby/proc.h>
#include <mruby/data.h>
#include <mruby/compile.h>
#include <mruby/variable.h>

void
mrb_init_mrbtest(mrb_state *);

/* Print a short remark for the user */
void print_hint(void)
{
  printf("mrbtest - Embeddable Ruby Test\n");
  printf("\nThis is a very early version, please test and report errors.\n");
  printf("Thanks :)\n\n");
}

static int
check_error(mrb_state *mrb)
{
  /* Error check */
  /* $ko_test and $kill_test should be 0 */
  mrb_value ko_test = mrb_gv_get(mrb, mrb_intern(mrb, "$ko_test"));
  mrb_value kill_test = mrb_gv_get(mrb, mrb_intern(mrb, "$kill_test"));

  return mrb_fixnum_p(ko_test) && mrb_fixnum(ko_test) == 0 && mrb_fixnum_p(kill_test) && mrb_fixnum(kill_test) == 0;
}

int
main(void)
{
  mrb_state *mrb;
  mrb_value return_value;
  const char *prog = "report()";
  int ret = EXIT_SUCCESS;

  print_hint();

  /* new interpreter instance */
  mrb = mrb_open();
  if (mrb == NULL) {
    fprintf(stderr, "Invalid mrb_state, exiting test driver");
    return EXIT_FAILURE;
  }

  mrb_init_mrbtest(mrb);
  /* evaluate the test */
  return_value = mrb_load_string(mrb, prog);
  /* did an exception occur? */
  if (mrb->exc) {
    mrb_p(mrb, return_value);
    mrb->exc = 0;
    ret = EXIT_FAILURE;
  }
  else if (!check_error(mrb)) {
    ret = EXIT_FAILURE;
  }
  mrb_close(mrb);

  return ret;
}
