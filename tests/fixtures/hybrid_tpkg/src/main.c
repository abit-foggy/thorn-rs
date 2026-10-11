/* hybrid_tpkg main - links the audited Rust archive. */
#include <stdio.h>

int probe_answer(void);   /* exported by out/libsafe.a */

int main(void)
{
    printf("probe_answer = %d\n", probe_answer());
    return probe_answer() == 42 ? 0 : 1;
}
