#pragma once

#include <ozo/result.h>
#include <ozo/error.h>
#include <ozo/type_traits.h>
#include <ozo/concept.h>
#include <ozo/detail/endian.h>
#include <ozo/detail/float.h>
#include <ozo/detail/array.h>
#include <ozo/istream.h>
#include <boost/core/demangle.hpp>
#include <boost/hana/for_each.hpp>
#include <boost/hana/members.hpp>

namespace ozo {
template <int... I>
constexpr std::tuple<boost::mpl::int_<I>...>
make_mpl_index_sequence(std::integer_sequence<int, I...>) {
    return {};
}

template <int N>
constexpr auto make_index_sequence(boost::mpl::int_<N>) {
    return make_mpl_index_sequence(
        std::make_integer_sequence<int, N>{}
    );
}

template <typename Adt, typename Index>
constexpr decltype(auto) member_name(const Adt&, const Index&) {
    return fusion::extension::struct_member_name<Adt, Index::value>::call();
}

template <typename Adt, typename Index>
constexpr decltype(auto) member_value(Adt&& v, const Index&) {
    return fusion::at<Index>(std::forward<Adt>(v));
}

template <typename Out, typename = std::void_t<>>
struct recv_impl{
    template <typename M>
    static istream& apply(istream& in, int32_t, const oid_map_t<M>&, Out& out) {
        return read(in, out);
    }
};

template <typename M, typename Out>
inline istream& recv(istream& in, int32_t size, const oid_map_t<M>& oids, Out& out) {
    if constexpr (!is_dynamic_size<Out>::value) {
        if (size != static_cast<int32_t>(size_of(out))) {
            throw std::range_error("data size " + std::to_string(size)
                + " does not match type size " + std::to_string(size_of(out)));
        }
    }
    return recv_impl<std::decay_t<Out>>::apply(in, size, oids, out);
}

template <>
struct recv_impl<std::string> {
    template <typename M>
    static istream& apply(istream& in, int32_t size, const oid_map_t<M>&, std::string& out) {
        out.resize(size);
        return read(in, out);
    }
};

template <typename Out>
struct recv_impl<std::vector<Out>> {
    using value_type = std::vector<Out>;

    template <typename M>
    static istream& apply(istream& in, int32_t, const oid_map_t<M>& oids, value_type& out) {
        detail::pg_array array_header;
        detail::pg_array_dimension dim_header;

        read(in, array_header);

        if (array_header.dimensions_count > 1) {
            throw std::range_error("multiply dimention count is not supported: "
                 + std::to_string(array_header.dimensions_count));
        }

        using item_type = typename value_type::value_type;

        if (!accepts_oid<item_type>(oids, array_header.elemtype)) {
            throw system_error(error::oid_type_mismatch,
                "unexpected oid " + std::to_string(array_header.elemtype)
                + " for element type of " + boost::core::demangle(typeid(out).name()));
        }

        if (array_header.dimensions_count < 1) {
            return in;
        }

        read(in, dim_header);

        if (dim_header.size == 0) {
            return in;
        }

        out.resize(dim_header.size);

        for (auto& item : out) {
            int32_t size = 0;
            read(in, size);
            if (size == -1) {
                if constexpr (!is_nullable<item_type>::value) {
                    throw std::range_error("unexpected NULL");
                }
            } else {
                if constexpr (!is_nullable<item_type>::value) {
                    recv(in, size, oids, item);
                } else {
                    throw std::range_error("arrays with nullable are not supported yet.");
                }
            }
        }
        return in;
    }
};

template <typename T, typename M, typename Out>
inline void recv(const value<T>& in, const oid_map_t<M>& oids, Out& out) {
    if (!accepts_oid(oids, out, in.oid())) {
        throw system_error(error::oid_type_mismatch, "unexpected oid "
            + std::to_string(in.oid()) + " for type "
            + boost::core::demangle(typeid(out).name()));
    }

    detail::istreambuf_view sbuf(in.data(), in.size());
    istream s(&sbuf);
    recv(s, in.size(), oids, out);
}

template <typename T, typename M, typename Out>
Require<FusionSequence<Out> && !FusionAdaptedStruct<Out>>
recv_row(const row<T>& in, const oid_map_t<M>& oid_map, Out& out) {

    if (static_cast<std::size_t>(fusion::size(out)) != std::size(in)) {
        throw std::range_error("row size " + std::to_string(std::size(in))
            + " does not match sequence " + boost::core::demangle(typeid(out).name())
            + " size " + std::to_string(fusion::size(out)));
    }

    auto i = in.begin();
    fusion::for_each(out, [&](auto& item) {
        recv(*i, oid_map, item);
        ++i;
    });
}

template <typename T, typename M, typename Out>
Require<FusionAdaptedStruct<Out>>
recv_row(const row<T>& in, const oid_map_t<M>& oid_map, Out& out) {

    if (static_cast<std::size_t>(fusion::size(out)) != std::size(in)) {
        throw std::range_error("row size " + std::to_string(std::size(in))
            + " does not match structure " + boost::core::demangle(typeid(out).name())
            + " size " + std::to_string(fusion::size(out)));
    }

    fusion::for_each(make_index_sequence(fusion::size(out)), [&](auto idx) {
        auto i = in.find(member_name(out, idx));
        if (i == in.end()) {
            throw std::range_error(std::string("row does not contain \"")
                + member_name(out, idx) + "\" column for "
                + boost::core::demangle(typeid(out).name()));
        } else {
            recv(*i, oid_map, member_value(out, idx));
        }
    });
}

template <typename T, typename M, typename Out>
Require<ForwardIterator<Out>>
recv_result(const basic_result<T>& in, const oid_map_t<M>& oid_map, Out out) {
    for (auto row : in) {
        recv_row(row, oid_map, *out++);
    }
}

template <typename T, typename M, typename Out>
Require<InsertIterator<Out>>
recv_result(const basic_result<T>& in, const oid_map_t<M>& oid_map, Out out) {
    for (auto row : in) {
        typename Out::container_type::value_type v{};
        recv_row(row, oid_map, v);
        *out++ = std::move(v);
    }
}

template <typename T, typename M>
void recv_result(basic_result<T>& in, const oid_map_t<M>&, basic_result<T>& out) {
    out = std::move(in);
}

} // namespace ozo