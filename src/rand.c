/*  Written in 2018 by David Blackman and Sebastiano Vigna (vigna@acm.org)

To the extent possible under law, the author has dedicated all copyright
and related and neighboring rights to this software to the public domain
worldwide. This software is distributed without any warranty.

See <http://creativecommons.org/publicdomain/zero/1.0/>. */

#include "common.h"
#include "rand.h"
#include "runtime.h"

#if defined(GIT_RAND_GETENTROPY)
# include <sys/random.h>
#endif

static uint64_t state[4];
static git_mutex state_lock;

/*
 * On 32-bit systems, we want to put inputs to the seed in the high
 * and low parts of the 64-bit seed, so we shift some into place.
 */
#ifdef GIT_ARCH_32
# define SEED_SHIFT 32
#else
# define SEED_SHIFT 0
#endif

#if defined(GIT_WIN32)
GIT_INLINE(int) getseed(uint64_t *seed)
{
	HCRYPTPROV provider;
	SYSTEMTIME systemtime;
	FILETIME filetime, idletime, kerneltime, usertime;

	if (CryptAcquireContext(&provider, 0, 0, PROV_RSA_FULL,
	                        CRYPT_VERIFYCONTEXT|CRYPT_SILENT)) {
		BOOL success = CryptGenRandom(provider, sizeof(uint64_t), (void *)seed);
		CryptReleaseContext(provider, 0);

		if (success)
			return 0;
	}

	GetSystemTime(&systemtime);
	if (!SystemTimeToFileTime(&systemtime, &filetime)) {
		git_error_set(GIT_ERROR_OS, "could not get time for random seed");
		return -1;
	}

	/* Fall-through: generate a seed from the time and system state */
	*seed = 0;
	*seed |= ((uint64_t)filetime.dwLowDateTime << 32);
	*seed |= ((uint64_t)filetime.dwHighDateTime);

	*seed ^= ((uint64_t)GetCurrentProcessId() << 32);
	*seed ^= ((uint64_t)GetCurrentThreadId());

	GetSystemTimes(&idletime, &kerneltime, &usertime);
	*seed |= ((uint64_t)idletime.dwLowDateTime);
	*seed |= ((uint64_t)idletime.dwHighDateTime << 32);
	*seed |= ((uint64_t)kerneltime.dwLowDateTime << 32);
	*seed |= ((uint64_t)kerneltime.dwHighDateTime);
	*seed |= ((uint64_t)usertime.dwLowDateTime);
	*seed |= ((uint64_t)usertime.dwHighDateTime << 32);

	/* Mix in the addresses of some functions and variables */
	*seed ^= (((uint64_t)((uintptr_t)getseed)) << SEED_SHIFT);
	*seed ^= (((uint64_t)((uintptr_t)seed)));
	*seed ^= (((uint64_t)((uintptr_t)printf)) << SEED_SHIFT);
	*seed ^= (((uint64_t)((uintptr_t)&errno)));

	*seed ^= ((uint64_t)git__timer());

	return 0;
}

#else

GIT_INLINE(int) getseed(uint64_t *seed)
{
	struct timeval tv;
	double loadavg[3];
	int fd;

# if defined(GIT_RAND_GETENTROPY)
	GIT_UNUSED((fd = 0));

	if (getentropy(seed, sizeof(uint64_t)) == 0)
		return 0;
# else
	/*
	 * Try to read from /dev/urandom; most modern systems will have
	 * this, but we may be chrooted, etc, so it's not a fatal error
	 */
	if ((fd = open("/dev/urandom", O_RDONLY)) >= 0) {
		ssize_t ret = read(fd, seed, sizeof(uint64_t));
		close(fd);

		if (ret == (ssize_t)sizeof(uint64_t))
			return 0;
	}
# endif

	/* Fall-through: generate a seed from the time and system state */
	*seed = 0;

	if (gettimeofday(&tv, NULL) < 0) {
		git_error_set(GIT_ERROR_OS, "could get time for random seed");
		return -1;
	}

	*seed = 0;
	*seed |= ((uint64_t)tv.tv_usec << 40);
	*seed |= ((uint64_t)tv.tv_sec);

	*seed ^= ((uint64_t)getpid() << 32);
	*seed ^= ((uint64_t)getpgid(0));
	*seed ^= ((uint64_t)getppid() << 32);
	*seed ^= ((uint64_t)getsid(0));
	*seed ^= ((uint64_t)getuid() << 32);
	*seed ^= ((uint64_t)getgid());

	*seed ^= ((uint64_t)loadavg[0]);
	*seed ^= ((uint64_t)loadavg[1]);
	*seed ^= ((uint64_t)loadavg[2]);

	/* Mix in the addresses of some functions and variables */
	*seed ^= (((uint64_t)((uintptr_t)getseed)) << SEED_SHIFT);
	*seed ^= (((uint64_t)((uintptr_t)seed)));
	*seed ^= (((uint64_t)((uintptr_t)printf)) << SEED_SHIFT);
	*seed ^= (((uint64_t)((uintptr_t)errno)));

	*seed ^= ((uint64_t)git__timer());

	return 0;
}
#endif

static void git_rand_global_shutdown(void)
{
	git_mutex_free(&state_lock);
}

int git_rand_global_init(void)
{
	uint64_t seed = 0;

	if (git_mutex_init(&state_lock) < 0 || getseed(&seed) < 0)
		return -1;

	if (!seed) {
		git_error_set(GIT_ERROR_INTERNAL, "failed to generate random seed");
		return -1;
	}

	git_rand_seed(seed);
	git_runtime_shutdown_register(git_rand_global_shutdown);

	return 0;
}

/*
 * This is splitmix64. xoroshiro256** uses 256 bit seed; this is used
 * to generate 256 bits of seed from the given 64, per the author's
 * recommendation.
 */
GIT_INLINE(uint64_t) splitmix64(uint64_t *in)
{
	uint64_t z;

	*in += 0x9e3779b97f4a7c15;

	z = *in;
	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9;
	z = (z ^ (z >> 27)) * 0x94d049bb133111eb;
	return z ^ (z >> 31);
}

void git_rand_seed(uint64_t seed)
{
	uint64_t mixer;

	mixer = seed;

	git_mutex_lock(&state_lock);
	state[0] = splitmix64(&mixer);
	state[1] = splitmix64(&mixer);
	state[2] = splitmix64(&mixer);
	state[3] = splitmix64(&mixer);
	git_mutex_unlock(&state_lock);
}

/* This is xoshiro256** 1.0, one of our all-purpose, rock-solid
   generators. It has excellent (sub-ns) speed, a state (256 bits) that is
   large enough for any parallel application, and it passes all tests we
   are aware of.

   For generating just floating-point numbers, xoshiro256+ is even faster.

   The state must be seeded so that it is not everywhere zero. If you have
   a 64-bit seed, we suggest to seed a splitmix64 generator and use its
   output to fill s. */

GIT_INLINE(uint64_t) rotl(const uint64_t x, int k) {
	return (x << k) | (x >> (64 - k));
}

uint64_t git_rand_next(void) {
	uint64_t t, result;

	git_mutex_lock(&state_lock);

	result = rotl(state[1] * 5, 7) * 9;

	t = state[1] << 17;

	state[2] ^= state[0];
	state[3] ^= state[1];
	state[1] ^= state[2];
	state[0] ^= state[3];

	state[2] ^= t;

	state[3] = rotl(state[3], 45);

	git_mutex_unlock(&state_lock);

	return result;
}
