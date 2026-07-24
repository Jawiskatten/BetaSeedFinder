public interface SearchListener {
    void onProgress(long checked, int matches, int topUpdates, double seedsPerSecond);

    default void onHit(long checked, int blocks) {
    }

    default void onHit(long checked, SearchResult result) {
        if (result != null) onHit(checked, result.blocks);
    }

    void onTopResult(SearchResult result, int rank);

    void onLog(String message);

    void onFinished();

    void onError(Throwable error);
}
