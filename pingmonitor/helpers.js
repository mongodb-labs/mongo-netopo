		// Move d to be adjacent to the cluster node.
		function cluster(alpha) {
		  var max = {};

		  // Find the largest node for each cluster.
		  nodes.forEach(function(d) {
			if (!(d.color in max) || (d.radius > max[d.color].radius)) {
			  max[d.color] = d;
			}
		  });

		  return function(d) {
			var node = max[d.color],
				l,
				r,
				x,
				y,
				k = 1,
				i = -1;

			// For cluster nodes, apply custom gravity.
			if (node == d) {
			  node = {x: w / 2, y: h / 2, radius: -d.radius};
			  k =.1 * Math.sqrt(d.radius);
			}

			x = d.x - node.x;
			y = d.y - node.y;
			l = Math.sqrt(x * x + y * y);
			r = d.radius + node.radius;
			if (l != r) {
			  l = (l - r) / l * alpha * k;
			  d.x -= x *= l;
			  d.y -= y *= l;
			  node.x += x;
			  node.y += y;
			}
		  };
		}