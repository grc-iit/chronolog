import React from 'react';

interface YouTubeProps {
  id: string;
}

export default function YouTube({id}: YouTubeProps): React.ReactElement {
  return (
    <div style={{position: 'relative', width: '100%', aspectRatio: '16/9', marginBottom: '1.5rem'}}>
      <iframe
        src={`https://www.youtube.com/embed/${id}`}
        title="YouTube video"
        allow="accelerometer; autoplay; clipboard-write; encrypted-media; gyroscope; picture-in-picture"
        allowFullScreen
        style={{position: 'absolute', top: 0, left: 0, width: '100%', height: '100%', border: 0}}
      />
    </div>
  );
}
