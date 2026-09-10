# Right now Redis supports 3 distance metrics for vector search

According to the [docs](https://redis.io/docs/latest/develop/ai/search-and-query/vectors/#distance-metrics), they are defined as such:

- L2 (Euclidean)
> (sum((ui - vi)^2))^(1/2)
Range: naturally unbounded

- IP
> 1 - u * v
Range: unbounded according to this formula

- COSINE
> 1 - ((u*v)/(|u||v|))
Range: [0, 2]

# Proposal

Add COSINE_SIMILARITY as a distance metric: formula ((u*v)/(|u||v|)) range: [-1, 1].

Rational:

- Industry standard with vector databases is `cosine_similarity` with range [-1, 1].
- Many customers existing downstream apps assume cosine_similarity so lack of support adds friction for replacement.
- Many ecosystem integrations also assume this convention and require us to reverse engineer the number for support.
- Vector distance metric doesn't intuitively express exact opposite vectors like a negative number does.

# Critique

If the formula for IP is implemented as specified in the docs, it's not technically inner product. It is one minus the inner product and just not normalized which makes the metric almost useless beyond sorting a list of search results.

# Steps

- Validate formulas presented and critique.
- Validate implementation vs documentation. If the documentation is wrong we can update but should be validated and update the spec.
- Research what is required to support the new distance_metric from an index creation and querying perspective.





