import hashlib
import random
import redis
import sys


def main(randomize=False):
    KEY = 'hllpy'
    r = redis.client.Redis()
    r.delete(KEY)
    jcnt = 0
    num_chunks = 100
    chunk_size = 1000
    while True:
        for j in xrange(num_chunks):
            if randomize:
                elements = [hashlib.sha1(str(i + jcnt) + '.' + str(random.randint(0, chunk_size))).hexdigest()
                            for i in xrange(chunk_size)]
            else:
                elements = [hashlib.sha1(str(i + jcnt)).hexdigest() for i in xrange(chunk_size)]
            r.execute_command('PFADD', KEY, *elements)
            jcnt += chunk_size
        approx = r.execute_command('PFCOUNT', KEY)
        err_pc = 100.0 * abs(jcnt - approx) / jcnt
        print '{} vs {}: {:5.4f}%'.format(jcnt, approx, err_pc)


if __name__ == '__main__':
    randomize = True if sys.argv[1:] else False
    try:
        main(randomize)
    except KeyboardInterrupt:
        pass
