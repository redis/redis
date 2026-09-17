# Implementation Plan: Add `COSINE_SIMILARITY` as a Fourth Vector Metric

## Goal

Add `COSINE_SIMILARITY` as a fourth accepted vector metric without changing the existing indexing or query execution approach.

The implementation should be non-breaking:

- Keep existing `L2`, `IP`, and `COSINE` behavior unchanged.
- Reuse the current `COSINE` execution path internally.
- Avoid introducing new ordering/comparator behavior in VecSim.
- Expose `COSINE_SIMILARITY` as a new public metric name with similarity-style output semantics.

## Alignment Review Against Existing Implementation

The plan should follow the code paths that already define and route vector metrics today.

### Public metric definition and parsing

- `modules/redisearch/src/deps/VectorSimilarity/src/VecSim/vec_sim_common.h`
  - Defines `VecSimMetric`.
- `modules/redisearch/src/src/vector_index.h`
  - Defines metric string constants such as `VECSIM_METRIC_IP`, `VECSIM_METRIC_L2`, `VECSIM_METRIC_COSINE`.
- `modules/redisearch/src/src/spec.c`
  - `parseVectorField_GetMetric()` parses `DISTANCE_METRIC` during `FT.CREATE`.
- `modules/redisearch/src/src/vector_index.c`
  - `VecSimMetric_ToString()` stringifies metric values.

These are the correct places to add the fourth metric name.

### Existing cosine execution path to reuse

- `modules/redisearch/src/deps/VectorSimilarity/src/VecSim/spaces/spaces.cpp`
  - Routes `VecSimMetric_Cosine` to the same low-level function family already used for cosine distance.
- `modules/redisearch/src/deps/VectorSimilarity/src/VecSim/index_factories/components/preprocessors_factory.h`
  - Applies cosine preprocessing/normalization.
- `modules/redisearch/src/src/iterators/hybrid_reader.c`
  - Normalizes hybrid query vectors when the metric is cosine.
- `modules/redisearch/src/deps/VectorSimilarity/src/VecSim/index_factories/svs_factory.cpp`
  - Routes `VecSimMetric_Cosine` through the existing SVS metric mapping.

These are the correct internal switch points to extend so that `COSINE_SIMILARITY` follows the same internal path as `COSINE`.

### Score/output handling points

- `modules/redisearch/src/src/vector_index.c`
  - `createMetricIteratorFromVectorQueryResults()` copies returned metric values into result iterators.
- `modules/redisearch/src/src/pipeline/pipeline_construction.c`
  - Selects metric-specific score normalization for hybrid execution.
- `modules/redisearch/src/src/vector_normalization.h`
  - Contains metric-specific normalization helpers.
- `modules/redisearch/src/src/result_processor.c`
  - Applies normalized vector scores in the pipeline.

These are the correct places to translate internal cosine-distance values into externally visible cosine-similarity values when needed.

## Execution Plan

## Phase 1: Add the Fourth Public Metric

### 1.1 Extend the metric enum

Update `modules/redisearch/src/deps/VectorSimilarity/src/VecSim/vec_sim_common.h` to add:

- `VecSimMetric_CosineSimilarity`

This preserves the current metric model and simply adds a fourth option.

### 1.2 Add the public metric string

Update `modules/redisearch/src/src/vector_index.h` to add:

- `VECSIM_METRIC_COSINE_SIMILARITY "COSINE_SIMILARITY"`

### 1.3 Parse and stringify the new metric

Update:

- `modules/redisearch/src/src/spec.c`
  - Extend `parseVectorField_GetMetric()`.
- `modules/redisearch/src/src/vector_index.c`
  - Extend `VecSimMetric_ToString()`.

This keeps schema creation and metadata output aligned with the existing pattern for the other three metrics.

## Phase 2: Reuse the Existing COSINE Internal Path

### 2.1 Route `COSINE_SIMILARITY` anywhere `COSINE` is already routed

Extend existing cosine branches so that `VecSimMetric_CosineSimilarity` is handled identically to `VecSimMetric_Cosine` in the internal execution path.

Primary files:

- `modules/redisearch/src/deps/VectorSimilarity/src/VecSim/spaces/spaces.cpp`
- `modules/redisearch/src/deps/VectorSimilarity/src/VecSim/index_factories/components/preprocessors_factory.h`
- `modules/redisearch/src/src/iterators/hybrid_reader.c`
- `modules/redisearch/src/deps/VectorSimilarity/src/VecSim/index_factories/svs_factory.cpp`

This means:

- The same normalization/preprocessing rules are reused.
- The same low-level cosine-distance functions are reused.
- The same index construction and search logic are reused.
- No new comparator or heap behavior is introduced.

### 2.2 Do not add new low-level metric math for VecSim search

Do **not** add new raw-similarity search functions in:

- `spaces/IP/IP.cpp`
- `spaces/IP/IP.h`
- heap/priority queue logic
- HNSW/BF ordering logic

Those changes would introduce a new search behavior model. They are unnecessary because cosine-distance ordering and cosine-similarity ordering are equivalent for KNN once you apply `similarity = 1 - distance` at the API boundary.

## Phase 3: Translate External Semantics at the Boundary

## Core rule

Internally:

- execute `COSINE_SIMILARITY` exactly like `COSINE`
- store/search/rank using cosine distance

Externally:

- present scores as cosine similarity
- accept cosine-similarity style thresholds where applicable

### 3.1 KNN queries

No ordering change is needed.

Reason:

- minimizing `1 - cosine_similarity`
- is equivalent to maximizing `cosine_similarity`

So the internal ranking stays unchanged.

### 3.2 Returned metric values

For fields defined with `COSINE_SIMILARITY`, convert the returned metric from cosine distance to cosine similarity:

- `similarity = 1 - distance`

Primary touch points:

- `modules/redisearch/src/src/vector_index.c`
  - for regular vector query replies returned through `createMetricIteratorFromVectorQueryResults()`
- `modules/redisearch/src/src/iterators/hybrid_reader.c`
  - for hybrid/ad-hoc distance computation paths

This is the key non-breaking adaptation: keep VecSim unchanged, translate only the exposed score.

### 3.3 Range queries

For `COSINE_SIMILARITY`, treat the user-provided threshold as a similarity threshold and convert it before calling the existing VecSim range query path:

- internal cosine distance radius = `1 - similarity_threshold`

Validation should enforce the public similarity range:

- allowed input range: `[-1, 1]`

Primary touch point:

- `modules/redisearch/src/src/vector_index.c`
  - before `VecSimIndex_RangeQuery(...)`

This preserves the existing range-query engine and only adapts the API semantics.

### 3.4 Hybrid score normalization

Hybrid normalization should continue using the existing metric-aware normalization pipeline.

Relevant files:

- `modules/redisearch/src/src/pipeline/pipeline_construction.c`
- `modules/redisearch/src/src/vector_normalization.h`
- `modules/redisearch/src/src/result_processor.c`

Implementation rule:

- do not invent a separate hybrid flow
- either map `VecSimMetric_CosineSimilarity` to the same normalization behavior as cosine distance
- or add a dedicated enum branch that reuses the same underlying transformation logic

Because the internal upstream value remains cosine distance until presentation, this should follow the existing cosine normalization pattern rather than introduce a new search-time calculation model.

Note: for hybrid queries, returning a hybrid score is expected and acceptable. `COSINE_SIMILARITY` does not need to force hybrid output fields into raw cosine-similarity `[-1, 1]` semantics when the query is intentionally using the hybrid scoring pipeline.

## Phase 4: Validation and Test Updates

Use existing test locations and patterns rather than creating a new test structure.

### 4.1 VecSim unit coverage

Update or extend:

- `modules/redisearch/src/deps/VectorSimilarity/tests/unit/test_spaces.cpp`
  - verify `VecSimMetric_CosineSimilarity` resolves through the same function-selection path as cosine
- `modules/redisearch/src/deps/VectorSimilarity/tests/unit/test_components.cpp`
  - verify preprocessing/normalization path matches cosine behavior

### 4.2 RediSearch integration coverage

Update or extend:

- `modules/redisearch/src/tests/pytests/test_vecsim.py`
  - `FT.CREATE` accepts `DISTANCE_METRIC COSINE_SIMILARITY`
  - KNN ordering matches cosine ordering
  - returned scores are cosine similarity values
  - range query threshold is interpreted as similarity and translated correctly
- `modules/redisearch/src/tests/pytests/test_hybrid_vector_normalizer.py`
  - hybrid normalization behaves correctly for the new metric

### 4.3 Backward-compatibility checks

Explicitly verify that:

- `COSINE` behavior remains unchanged
- `IP` behavior remains unchanged
- `L2` behavior remains unchanged
- existing query ordering and score semantics for prior metrics do not regress

## Phase 5: Documentation

Document only the externally visible addition:

- `COSINE` returns cosine distance: `1 - cosine_similarity`
- `COSINE_SIMILARITY` returns cosine similarity directly: `cosine_similarity`
- both use the same internal cosine execution path

This should be documented as a new metric name and output convention, not as a new indexing/search algorithm.

## Latency Considerations

The intended implementation keeps latency risk low because it reuses the existing cosine indexing and search path.

- No new search algorithm is introduced.
- No heap/comparator/order changes are introduced in VecSim.
- KNN execution keeps the same internal cosine-distance ranking behavior.
- Range queries add only a constant-time threshold translation from similarity to distance.
- Non-hybrid result reporting may add a small per-result conversion from distance to similarity (`1 - distance`).

Potential impact should therefore be limited to lightweight boundary translation work on returned results, which is expected to be negligible relative to vector search itself. The main thing to validate is that any result rewriting happens only at the output boundary and does not add extra preprocessing or extra distance computations inside the hot search path.

## Non-Breaking Guardrails

The implementation should preserve these constraints:

1. No changes to existing metric semantics.
2. No new VecSim heap/comparator/order model.
3. No new low-level cosine-similarity search math for HNSW/BF.
4. `COSINE_SIMILARITY` should be implemented by aliasing the internal cosine path and translating public inputs/outputs.
5. `modules/vector-sets/` is out of scope unless product requirements explicitly say this new metric must also exist for `VSET.*` commands.
