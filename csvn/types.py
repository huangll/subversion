from csvn.core import *
from csvn.ext.listmixin import ListMixin
from UserDict import DictMixin

# This class contains Pythonic wrappers for generic Subversion and
# APR datatypes (e.g. dates, streams, hashes, arrays, etc).
#
# These wrappers are used by higher-level APIs in csvn to wrap
# raw C datatypes in a way that is easy for Python developers
# to use.

class SvnDate(str):

    def as_apr_time_t(self):
        """Return this date to an apr_time_t object"""
        pool = Pool()
        when = apr_time_t()
        svn_time_from_cstring(byref(when), self, pool)
        return when

    def as_human_string(self):
        """Return this date to a human-readable date"""
        pool = Pool()
        return str(svn_time_to_human_cstring(self.as_apr_time_t(), pool))

class Hash(DictMixin):
    """A dictionary wrapper for apr_hash_t"""

    def __init__(self, type, items={}, wrapper=None, dup=None):
        self.type = type
        self.pool = Pool()
        self.wrapper = wrapper
        if isinstance(items, POINTER(apr_hash_t)):
            if dup:
                self.hash = apr_hash_copy(self.pool, items)
            else:
                self.hash = items
        elif items is None:
            self.hash = POINTER(apr_hash_t)()
        elif isinstance(items, Hash):
            if dup:
                self.hash = apr_hash_copy(self.pool, items)
            else:
                self.hash = items.hash
        else:
            self.hash = apr_hash_make(self.pool)
            if hasattr(items, "iteritems"):
                self.update(items.iteritems())
            else:
                self.update(items)

        if dup:
            # Copy items into our pool
            for key, value in self.iteritems():
                self[key] = dup(value, self.pool)

    def __getitem__(self, key):
        value = apr_hash_get(self, cast(key, c_void_p), len(key))
        if not value:
            raise KeyError(key)
        value = cast(value, self.type)
        if self.wrapper:
            value = self.wrapper.from_param(value)
        return value

    def __setitem__(self, key, value):
        if self.wrapper:
            value = self.wrapper.to_param(value, self.pool)
        apr_hash_set(self, key, len(key), value)

    def __delitem__(self, key):
        apr_hash_set(self, key, len(key), NULL)

    def keys(self):
        return list(self.iterkeys())

    def __iter__(self):
        for (key, _) in self.iteritems():
            yield key

    def iteritems(self):
        pool = Pool()
        hi = apr_hash_first(pool, self)
        while hi:
            key_vp = c_void_p()
            val_vp = c_void_p()
            apr_hash_this(hi, byref(key_vp), None, byref(val_vp))
            val = cast(val_vp, self.type)
            if self.wrapper:
                val = self.wrapper.from_param(val)
            yield (string_at(key_vp), val)
            hi = apr_hash_next(hi)

    def __len__(self):
        return int(apr_hash_count(self))

    def byref(self):
        return byref(self._as_parameter_)

    def pointer(self):
        return pointer(self._as_parameter_)

    _as_parameter_ = property(fget=lambda self: self.hash)


class Array(ListMixin):
    """An array wrapper for apr_array_header_t"""

    def __init__(self, type, items=None, size=0):
        self.type = type
        self.pool = Pool()
        if not items:
            self.header = apr_array_make(self.pool, size, sizeof(type))
        elif isinstance(items, POINTER(apr_array_header_t)):
            self.header = items
        elif isinstance(items, Array):
            self.header = apr_array_copy(self.pool, items)
        else:
            self.header = apr_array_make(self.pool, len(items),
                                         sizeof(type))
            self.extend(items)

    _as_parameter_ = property(fget=lambda self: self.header)
    elts = property(fget=lambda self: cast(self.header[0].elts.raw,
                                           POINTER(self.type)))

    def _get_element(self, i):
        return self.elts[i]

    def _set_element(self, i, value):
        self.elts[i] = value

    def __len__(self):
        return self.header[0].nelts

    def _resize_region(self, start, end, new_size):
        diff = start-end+new_size

        # Growing
        if diff > 0:
            l = len(self)

            # Make space for the new items
            for i in xrange(diff):
                apr_array_push(self)

            # Move the old items out of the way, if necessary
            if end < l:
                src_idx = max(end-diff,0)
                memmove(byref(self.elts + end),
                        byref(self.elts[src_idx]),
                        (l-src_idx)*self.header[0].elt_size)

        # Shrinking
        elif diff < 0:

            # Overwrite the deleted items with items we still need
            if end < len(self):
                memmove(byref(self.elts[end+diff]),
                        byref(self.elts[end]),
                        (len(self)-end)*self.header[0].elt_size)

            # Shrink the array
            for i in xrange(-diff):
                apr_array_pop(self)

def _stream_read(baton, buffer, l):
    f = cast(baton, py_object).value
    s = f.read(l[0])
    memmove(buffer, string, len(s))
    l[0] = len(s)
    return SVN_NO_ERROR

def _stream_write(baton, data, l):
    f = cast(baton, py_object).value
    s = string_at(data.raw, l[0])
    f.write(s)
    return SVN_NO_ERROR

def _stream_close(baton):
    f = cast(baton, py_object).value
    f.close()
    return SVN_NO_ERROR

class Stream(object):

    def __init__(self, buffer, disown=False):
        """Create a stream which wraps a Python file or file-like object"""

        self.pool = Pool()
        self.buffer = buffer
        baton = c_void_p(id(buffer))
        self.stream = svn_stream_create(baton, self.pool)
        svn_stream_set_read(self.stream, svn_read_fn_t(_stream_read))
        svn_stream_set_write(self.stream, svn_write_fn_t(_stream_write))
        if not disown:
            svn_stream_set_close(self.stream, svn_close_fn_t(_stream_close))

    _as_parameter_ = property(fget=lambda self: self.stream)

class SvnStringPtr(object):

    def to_param(obj, pool):
        return svn_string_ncreate(obj, len(obj), pool)
    to_param = staticmethod(to_param)

    def from_param(obj):

        assert isinstance(obj[0], svn_string_t)

        # Convert from a raw svn_string_t object. Pass in the length, so that
        # we handle binary property values with embedded NULLs correctly.
        return string_at(obj[0].data.raw, obj[0].len)
    from_param = staticmethod(from_param)

