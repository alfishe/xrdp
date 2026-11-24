#if defined(HAVE_CONFIG_H)
#include "config_ac.h"
#endif

#include <stdlib.h>
#include <check.h>

Suite *make_suite_test_sound_latency(void);

int
main(void)
{
    int number_failed;
    SRunner *sr;

    sr = srunner_create(make_suite_test_sound_latency());
    srunner_set_tap(sr, "-");
    srunner_run_all(sr, CK_ENV);
    number_failed = srunner_ntests_failed(sr);
    srunner_free(sr);
    return (number_failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
