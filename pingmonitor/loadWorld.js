function loadWorldMap( type , width , height ){

	var color = d3.scale.category10();
	
	svgWorld = d3.select( type ).append("svg")
		.attr("width", width)
		.attr("height", height)
		.attr("id", "svgWorld");

	projection = d3.geo.equirectangular();

	var path = d3.geo.path()
		.projection(projection);

	var graticule = d3.geo.graticule();

	svgWorld.append("path")
		.datum(graticule)
		.attr("class", "graticule")
		.attr("d", path);

	d3.json("world-50m.json", function(error, world) {
	  svgWorld.insert("path", ".graticule")
		  .datum(topojson.feature(world, world.objects.land))
		  .attr("class", "land")
		  .attr("d", path);

	  svgWorld.insert("path", ".graticule")
		  .datum(topojson.mesh(world, world.objects.countries, function(a, b) { return a !== b; }))
		  .attr("class", "boundary")
		  .attr("d", path);
		});
}